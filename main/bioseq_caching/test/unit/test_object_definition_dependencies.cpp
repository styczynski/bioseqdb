#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

import bioseq.cache.core;

namespace {

using bioseq::cache::DefinitionPhysicalChangeV1;
using bioseq::cache::DependencyKey;
using bioseq::cache::DependencyKind;
using bioseq::cache::ObjectDefinitionDependencyCollectorV1;
using bioseq::cache::ObjectDefinitionDependencyErrorV1;
using bioseq::cache::ObjectDefinitionMutationV1;
using bioseq::cache::ObjectDefinitionObservationV1;
using bioseq::cache::PhysicalIdentityKind;
using bioseq::cache::PhysicalIdentityV1;
using bioseq::cache::database_definition_guard_key_v1;
using bioseq::cache::database_definition_guard_key_valid_v1;
using bioseq::cache::dependency_key_less_v1;
using bioseq::cache::kMaximumObjectDefinitionSpecificDependenciesV1;
using bioseq::cache::make_object_definition_mutation_v1;
using bioseq::cache::make_syscache_definition_mutation_v1;
using bioseq::cache::object_definition_dependency_key_valid_v1;
using bioseq::cache::object_definition_mutation_valid_v1;

[[nodiscard]] constexpr DependencyKey object_key(
    DependencyKind kind, std::uint32_t object_oid,
    std::uint32_t class_oid = 1255) noexcept {
  return DependencyKey{
      .database_oid = 17,
      .class_oid = class_oid,
      .object_oid = object_oid,
      .object_sub_id = 0,
      .kind = kind,
      .reserved = 0,
  };
}

[[nodiscard]] constexpr PhysicalIdentityV1 physical_identity(
    std::uint8_t marker) noexcept {
  PhysicalIdentityV1 result{
      .kind = PhysicalIdentityKind::ProviderOpaque,
      .length = 1,
  };
  result.bytes[0] = static_cast<std::byte>(marker);
  return result;
}

[[nodiscard]] const ObjectDefinitionMutationV1 *find_kind(
    std::span<const ObjectDefinitionMutationV1> mutations,
    DependencyKind kind) noexcept {
  for (const ObjectDefinitionMutationV1 &mutation : mutations) {
    if (mutation.key.kind == kind) {
      return &mutation;
    }
  }
  return nullptr;
}

static_assert(std::is_trivially_copyable_v<ObjectDefinitionMutationV1>);
static_assert(std::is_standard_layout_v<ObjectDefinitionMutationV1>);
static_assert(kMaximumObjectDefinitionSpecificDependenciesV1 + 2U == 256U);

TEST_CASE("object dependency keys cover every definition class and syscache",
          "[cache][unit][object-definition][keys]") {
  constexpr std::array kinds{
      DependencyKind::Procedure,
      DependencyKind::Operator,
      DependencyKind::Type,
      DependencyKind::Cast,
      DependencyKind::Collation,
      DependencyKind::Policy,
      DependencyKind::Role,
  };
  std::uint32_t object_oid = 100;
  for (const DependencyKind kind : kinds) {
    const ObjectDefinitionMutationV1 mutation =
        make_object_definition_mutation_v1(
            object_key(kind, object_oid++));
    REQUIRE(object_definition_dependency_key_valid_v1(mutation.key));
    REQUIRE(object_definition_mutation_valid_v1(mutation));
    REQUIRE(mutation.physical_change ==
            DefinitionPhysicalChangeV1::Unchanged);
  }

  const ObjectDefinitionMutationV1 syscache =
      make_syscache_definition_mutation_v1(17, 0, 0);
  REQUIRE(syscache.key.kind == DependencyKind::SyscacheHash);
  REQUIRE(syscache.key.class_oid == 0);
  REQUIRE(syscache.key.object_oid == 0);
  REQUIRE(object_definition_mutation_valid_v1(syscache));

  const DependencyKey guard = database_definition_guard_key_v1(17);
  REQUIRE(database_definition_guard_key_valid_v1(guard));
  REQUIRE_FALSE(object_definition_dependency_key_valid_v1(guard));
}

TEST_CASE("definition collector deduplicates and copies canonical guard plus keys",
          "[cache][unit][object-definition][canonical]") {
  ObjectDefinitionDependencyCollectorV1<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          ObjectDefinitionDependencyErrorV1::None);

  const ObjectDefinitionMutationV1 type =
      make_object_definition_mutation_v1(
          object_key(DependencyKind::Type, 303, 1247));
  const ObjectDefinitionMutationV1 procedure =
      make_object_definition_mutation_v1(
          object_key(DependencyKind::Procedure, 101));
  const ObjectDefinitionMutationV1 syscache =
      make_syscache_definition_mutation_v1(17, 7, 99);
  REQUIRE(collector.observe(1, type).observation ==
          ObjectDefinitionObservationV1::Recorded);
  REQUIRE(collector.observe(1, procedure).observation ==
          ObjectDefinitionObservationV1::Recorded);
  REQUIRE(collector.observe(1, syscache).observation ==
          ObjectDefinitionObservationV1::Recorded);
  const auto duplicate = collector.observe(1, type);
  REQUIRE(duplicate.observation == ObjectDefinitionObservationV1::Duplicate);
  REQUIRE(duplicate.root_guard_required);
  REQUIRE_FALSE(duplicate.precise_runtime_update_required);
  REQUIRE(collector.observation_count() == 3);

  std::array<ObjectDefinitionMutationV1, 9> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result);
  REQUIRE(result.count == 4);
  REQUIRE_FALSE(result.guard_only);
  REQUIRE(database_definition_guard_key_valid_v1(copied[0].key));
  for (std::size_t index = 1; index < result.count; ++index) {
    REQUIRE(dependency_key_less_v1(
        copied[index - 1U].key, copied[index].key));
  }
}

TEST_CASE("rolled-back savepoint removes ordinary definition events",
          "[cache][unit][object-definition][savepoint]") {
  ObjectDefinitionDependencyCollectorV1<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.begin_savepoint(2, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.observe(
              2, make_object_definition_mutation_v1(
                     object_key(DependencyKind::Procedure, 101)))) ;
  REQUIRE(collector.finish_savepoint(2, 1, false) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE_FALSE(collector.transaction_dirty());
  REQUIRE(collector.observation_count() == 0);

  std::array<ObjectDefinitionMutationV1, 4> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result);
  REQUIRE(result.count == 0);
}

TEST_CASE("released savepoint composes old and current physical identity",
          "[cache][unit][object-definition][savepoint][physical]") {
  ObjectDefinitionDependencyCollectorV1<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  const DependencyKey procedure = object_key(DependencyKind::Procedure, 101);
  const PhysicalIdentityV1 identity_a = physical_identity(1);
  const PhysicalIdentityV1 identity_b = physical_identity(2);
  const PhysicalIdentityV1 identity_c = physical_identity(3);

  REQUIRE(collector.observe(
      1, make_object_definition_mutation_v1(
             procedure, identity_a, identity_b)));
  REQUIRE(collector.begin_savepoint(2, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.observe(
      2, make_object_definition_mutation_v1(
             procedure, identity_b, identity_c)));
  REQUIRE(collector.finish_savepoint(2, 1, true) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.observation_count() == 1);

  std::array<ObjectDefinitionMutationV1, 4> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result.count == 2);
  const ObjectDefinitionMutationV1 *composed = find_kind(
      std::span<const ObjectDefinitionMutationV1>(copied.data(), result.count),
      DependencyKind::Procedure);
  REQUIRE(composed != nullptr);
  REQUIRE(composed->previous_physical_identity == identity_a);
  REQUIRE(composed->current_physical_identity == identity_c);
  REQUIRE(composed->physical_change ==
          DefinitionPhysicalChangeV1::Replaced);
}

TEST_CASE("savepoint rollback restores the parent physical transition",
          "[cache][unit][object-definition][savepoint][physical]") {
  ObjectDefinitionDependencyCollectorV1<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  const DependencyKey procedure = object_key(DependencyKind::Procedure, 101);
  const PhysicalIdentityV1 identity_a = physical_identity(1);
  const PhysicalIdentityV1 identity_b = physical_identity(2);
  const PhysicalIdentityV1 identity_c = physical_identity(3);
  REQUIRE(collector.observe(
      1, make_object_definition_mutation_v1(
             procedure, identity_a, identity_b)));
  REQUIRE(collector.begin_savepoint(2, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.observe(
      2, make_object_definition_mutation_v1(
             procedure, identity_b, identity_c)));
  REQUIRE(collector.finish_savepoint(2, 1, false) ==
          ObjectDefinitionDependencyErrorV1::None);

  std::array<ObjectDefinitionMutationV1, 4> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  const ObjectDefinitionMutationV1 *retained = find_kind(
      std::span<const ObjectDefinitionMutationV1>(copied.data(), result.count),
      DependencyKind::Procedure);
  REQUIRE(retained != nullptr);
  REQUIRE(retained->previous_physical_identity == identity_a);
  REQUIRE(retained->current_physical_identity == identity_b);
}

TEST_CASE("ambiguous physical history remains guard-only after rollback",
          "[cache][unit][object-definition][savepoint][fallback]") {
  ObjectDefinitionDependencyCollectorV1<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.begin_savepoint(2, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  const DependencyKey procedure = object_key(DependencyKind::Procedure, 101);
  REQUIRE(collector.observe(
      2, make_object_definition_mutation_v1(
             procedure, physical_identity(1), physical_identity(2))));
  const auto ambiguous = collector.observe(
      2, make_object_definition_mutation_v1(
             procedure, physical_identity(3), physical_identity(4)));
  REQUIRE(ambiguous.error ==
          ObjectDefinitionDependencyErrorV1::AmbiguousMutation);
  REQUIRE(ambiguous.observation ==
          ObjectDefinitionObservationV1::DatabaseDefinitionGuardOnly);
  REQUIRE(ambiguous.root_guard_required);
  REQUIRE(collector.guard_only());
  REQUIRE(collector.finish_savepoint(2, 1, false) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.transaction_dirty());

  std::array<ObjectDefinitionMutationV1, 4> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result.count == 1);
  REQUIRE(result.guard_only);
  REQUIRE(database_definition_guard_key_valid_v1(copied[0].key));
}

TEST_CASE("specific capacity and output pressure fail closed to one guard",
          "[cache][unit][object-definition][capacity]") {
  SECTION("specific-key pressure") {
    ObjectDefinitionDependencyCollectorV1<2, 4> collector;
    REQUIRE(collector.initialize(17, 1) ==
            ObjectDefinitionDependencyErrorV1::None);
    REQUIRE(collector.observe(
        1, make_object_definition_mutation_v1(
               object_key(DependencyKind::Procedure, 101))));
    REQUIRE(collector.observe(
        1, make_object_definition_mutation_v1(
               object_key(DependencyKind::Type, 102, 1247))));
    const auto overflow = collector.observe(
        1, make_object_definition_mutation_v1(
               object_key(DependencyKind::Operator, 103, 2617)));
    REQUIRE(overflow.error ==
            ObjectDefinitionDependencyErrorV1::CapacityExceeded);
    REQUIRE(collector.guard_only());

    std::array<ObjectDefinitionMutationV1, 3> copied{};
    const auto result = collector.copy_committed_dependencies(copied);
    REQUIRE(result.count == 1);
    REQUIRE(result.guard_only);
    REQUIRE(database_definition_guard_key_valid_v1(copied[0].key));
  }

  SECTION("destination pressure") {
    ObjectDefinitionDependencyCollectorV1<4, 4> collector;
    REQUIRE(collector.initialize(17, 1) ==
            ObjectDefinitionDependencyErrorV1::None);
    REQUIRE(collector.observe(
        1, make_object_definition_mutation_v1(
               object_key(DependencyKind::Procedure, 101))));
    REQUIRE(collector.observe(
        1, make_object_definition_mutation_v1(
               object_key(DependencyKind::Type, 102, 1247))));
    std::array<ObjectDefinitionMutationV1, 2> copied{};
    const auto result = collector.copy_committed_dependencies(copied);
    REQUIRE(result.error ==
            ObjectDefinitionDependencyErrorV1::DestinationTooSmall);
    REQUIRE(result.count == 1);
    REQUIRE(result.required_count == 3);
    REQUIRE(result.guard_only);
    REQUIRE(collector.guard_only());
    REQUIRE(database_definition_guard_key_valid_v1(copied[0].key));
  }
}

TEST_CASE("child capacity overflow keeps the root guard after rollback",
          "[cache][unit][object-definition][capacity][savepoint]") {
  ObjectDefinitionDependencyCollectorV1<2, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.begin_savepoint(2, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.observe(
      2, make_object_definition_mutation_v1(
             object_key(DependencyKind::Procedure, 101))));
  REQUIRE(collector.observe(
      2, make_object_definition_mutation_v1(
             object_key(DependencyKind::Type, 102, 1247))));
  const auto overflow = collector.observe(
      2, make_object_definition_mutation_v1(
             object_key(DependencyKind::Operator, 103, 2617)));
  REQUIRE(overflow.error ==
          ObjectDefinitionDependencyErrorV1::CapacityExceeded);
  REQUIRE(collector.finish_savepoint(2, 1, false) ==
          ObjectDefinitionDependencyErrorV1::None);
  REQUIRE(collector.guard_only());
  REQUIRE(collector.transaction_dirty());

  std::array<ObjectDefinitionMutationV1, 3> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result);
  REQUIRE(result.guard_only);
  REQUIRE(result.count == 1);
  REQUIRE(database_definition_guard_key_valid_v1(copied[0].key));
}

TEST_CASE("definition capture reserves both database epoch references",
          "[cache][unit][object-definition][capacity][canonical]") {
  ObjectDefinitionDependencyCollectorV1<> collector;
  REQUIRE(collector.initialize(17, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  for (std::uint32_t index = 0;
       index < kMaximumObjectDefinitionSpecificDependenciesV1; ++index) {
    REQUIRE(collector.observe(
        1, make_object_definition_mutation_v1(
               object_key(DependencyKind::Procedure, 1'000U + index))));
  }
  std::array<ObjectDefinitionMutationV1, 256> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result);
  REQUIRE_FALSE(result.guard_only);
  REQUIRE(result.count ==
          kMaximumObjectDefinitionSpecificDependenciesV1 + 1U);
  REQUIRE(result.required_count == result.count);
  REQUIRE(std::any_of(
      copied.begin(), copied.end(),
      [](const ObjectDefinitionMutationV1 &mutation) noexcept {
        return database_definition_guard_key_valid_v1(mutation.key);
      }));
}

TEST_CASE("invalid keys and savepoint tracking failures retain the guard",
          "[cache][unit][object-definition][fallback]") {
  SECTION("unsupported key") {
    ObjectDefinitionDependencyCollectorV1<4, 4> collector;
    REQUIRE(collector.initialize(17, 1) ==
            ObjectDefinitionDependencyErrorV1::None);
    const auto invalid = collector.observe(
        1, make_object_definition_mutation_v1(
               object_key(DependencyKind::RelationContent, 101)));
    REQUIRE(invalid.error ==
            ObjectDefinitionDependencyErrorV1::AmbiguousMutation);
    REQUIRE(invalid.root_guard_required);
    REQUIRE(invalid.guard_only);
  }

  SECTION("wrong database") {
    ObjectDefinitionDependencyCollectorV1<4, 4> collector;
    REQUIRE(collector.initialize(17, 1) ==
            ObjectDefinitionDependencyErrorV1::None);
    DependencyKey wrong_database =
        object_key(DependencyKind::Procedure, 101);
    wrong_database.database_oid = 18;
    const auto invalid = collector.observe(
        1, make_object_definition_mutation_v1(wrong_database));
    REQUIRE(invalid.error ==
            ObjectDefinitionDependencyErrorV1::InvalidArgument);
    REQUIRE(collector.guard_only());
  }

  SECTION("frame pressure") {
    ObjectDefinitionDependencyCollectorV1<4, 1> collector;
    REQUIRE(collector.initialize(17, 1) ==
            ObjectDefinitionDependencyErrorV1::None);
    REQUIRE(collector.begin_savepoint(2, 1) ==
            ObjectDefinitionDependencyErrorV1::CapacityExceeded);
    REQUIRE(collector.guard_only());
    REQUIRE(collector.transaction_dirty());
  }
}

TEST_CASE("reset removes the sticky guard and all transaction state",
          "[cache][unit][object-definition][reset]") {
  ObjectDefinitionDependencyCollectorV1<2, 2> collector;
  REQUIRE(collector.initialize(17, 1) ==
          ObjectDefinitionDependencyErrorV1::None);
  collector.force_database_definition_guard();
  REQUIRE(collector.guard_only());
  collector.reset();
  REQUIRE_FALSE(collector.guard_only());
  REQUIRE_FALSE(collector.transaction_dirty());
  REQUIRE(collector.observation_count() == 0);
  REQUIRE(collector.frame_count() == 0);
}

}  // namespace
