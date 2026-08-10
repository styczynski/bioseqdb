#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

import bioseq.cache.core;

namespace {

using bioseq::cache::definition_physical_change;
using bioseq::cache::dependency_key;
using bioseq::cache::dependency_kind;
using bioseq::cache::object_definition_dependency_collector;
using bioseq::cache::object_definition_dependency_error;
using bioseq::cache::object_definition_mutation;
using bioseq::cache::object_definition_observation;
using bioseq::cache::physical_identity_kind;
using bioseq::cache::physical_identity;
using bioseq::cache::database_definition_guard_key;
using bioseq::cache::database_definition_guard_key_valid;
using bioseq::cache::dependency_key_less;
using bioseq::cache::k_maximum_object_definition_specific_dependencies;
using bioseq::cache::make_object_definition_mutation;
using bioseq::cache::make_syscache_definition_mutation;
using bioseq::cache::object_definition_dependency_key_valid;
using bioseq::cache::object_definition_mutation_valid;

[[nodiscard]] constexpr dependency_key object_key(
    dependency_kind kind, std::uint32_t object_oid,
    std::uint32_t class_oid = 1255) noexcept {
  return dependency_key{
      .database_oid = 17,
      .class_oid = class_oid,
      .object_oid = object_oid,
      .object_sub_id = 0,
      .kind = kind,
      .reserved = 0,
  };
}

[[nodiscard]] constexpr physical_identity physical_identity(
    std::uint8_t marker) noexcept {
  physical_identity result{
      .kind = physical_identity_kind::provider_opaque,
      .length = 1,
  };
  result.bytes[0] = static_cast<std::byte>(marker);
  return result;
}

[[nodiscard]] const object_definition_mutation *find_kind(
    std::span<const object_definition_mutation> mutations,
    dependency_kind kind) noexcept {
  for (const object_definition_mutation &mutation : mutations) {
    if (mutation.key.kind == kind) {
      return &mutation;
    }
  }
  return nullptr;
}

static_assert(std::is_trivially_copyable_v<object_definition_mutation>);
static_assert(std::is_standard_layout_v<object_definition_mutation>);
static_assert(k_maximum_object_definition_specific_dependencies + 2U == 256U);

TEST_CASE("object dependency keys cover every definition class and syscache",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  constexpr std::array kinds{
      dependency_kind::procedure,
      dependency_kind::operator_object,
      dependency_kind::type,
      dependency_kind::cast,
      dependency_kind::collation,
      dependency_kind::policy,
      dependency_kind::role,
  };
  std::uint32_t object_oid = 100;
  for (const dependency_kind kind : kinds) {
    const object_definition_mutation mutation =
        make_object_definition_mutation(
            object_key(kind, object_oid++));
    REQUIRE(object_definition_dependency_key_valid(mutation.key));
    REQUIRE(object_definition_mutation_valid(mutation));
    REQUIRE(mutation.physical_change ==
            definition_physical_change::unchanged);
  }

  const object_definition_mutation syscache =
      make_syscache_definition_mutation(17, 0, 0);
  REQUIRE(syscache.key.kind == dependency_kind::syscache_hash);
  REQUIRE(syscache.key.class_oid == 0);
  REQUIRE(syscache.key.object_oid == 0);
  REQUIRE(object_definition_mutation_valid(syscache));

  const dependency_key guard = database_definition_guard_key(17);
  REQUIRE(database_definition_guard_key_valid(guard));
  REQUIRE_FALSE(object_definition_dependency_key_valid(guard));
}

TEST_CASE("definition collector deduplicates and copies canonical guard plus keys",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  object_definition_dependency_collector<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          object_definition_dependency_error::none);

  const object_definition_mutation type =
      make_object_definition_mutation(
          object_key(dependency_kind::type, 303, 1247));
  const object_definition_mutation procedure =
      make_object_definition_mutation(
          object_key(dependency_kind::procedure, 101));
  const object_definition_mutation syscache =
      make_syscache_definition_mutation(17, 7, 99);
  REQUIRE(collector.observe(1, type).observation ==
          object_definition_observation::recorded);
  REQUIRE(collector.observe(1, procedure).observation ==
          object_definition_observation::recorded);
  REQUIRE(collector.observe(1, syscache).observation ==
          object_definition_observation::recorded);
  const auto duplicate = collector.observe(1, type);
  REQUIRE(duplicate.observation == object_definition_observation::duplicate);
  REQUIRE(duplicate.root_guard_required);
  REQUIRE_FALSE(duplicate.precise_runtime_update_required);
  REQUIRE(collector.observation_count() == 3);

  std::array<object_definition_mutation, 9> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result);
  REQUIRE(result.count == 4);
  REQUIRE_FALSE(result.guard_only);
  REQUIRE(database_definition_guard_key_valid(copied[0].key));
  for (std::size_t index = 1; index < result.count; ++index) {
    REQUIRE(dependency_key_less(
        copied[index - 1U].key, copied[index].key));
  }
}

TEST_CASE("rolled-back savepoint removes ordinary definition events",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  object_definition_dependency_collector<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.begin_savepoint(2, 1) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.observe(
              2, make_object_definition_mutation(
                     object_key(dependency_kind::procedure, 101)))) ;
  REQUIRE(collector.finish_savepoint(2, 1, false) ==
          object_definition_dependency_error::none);
  REQUIRE_FALSE(collector.transaction_dirty());
  REQUIRE(collector.observation_count() == 0);

  std::array<object_definition_mutation, 4> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result);
  REQUIRE(result.count == 0);
}

TEST_CASE("released savepoint composes old and current physical identity",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  object_definition_dependency_collector<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          object_definition_dependency_error::none);
  const dependency_key procedure = object_key(dependency_kind::procedure, 101);
  const physical_identity identity_a = physical_identity(1);
  const physical_identity identity_b = physical_identity(2);
  const physical_identity identity_c = physical_identity(3);

  REQUIRE(collector.observe(
      1, make_object_definition_mutation(
             procedure, identity_a, identity_b)));
  REQUIRE(collector.begin_savepoint(2, 1) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.observe(
      2, make_object_definition_mutation(
             procedure, identity_b, identity_c)));
  REQUIRE(collector.finish_savepoint(2, 1, true) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.observation_count() == 1);

  std::array<object_definition_mutation, 4> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result.count == 2);
  const object_definition_mutation *composed = find_kind(
      std::span<const object_definition_mutation>(copied.data(), result.count),
      dependency_kind::procedure);
  REQUIRE(composed != nullptr);
  REQUIRE(composed->previous_physical_identity == identity_a);
  REQUIRE(composed->current_physical_identity == identity_c);
  REQUIRE(composed->physical_change ==
          definition_physical_change::replaced);
}

TEST_CASE("savepoint rollback restores the parent physical transition",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  object_definition_dependency_collector<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          object_definition_dependency_error::none);
  const dependency_key procedure = object_key(dependency_kind::procedure, 101);
  const physical_identity identity_a = physical_identity(1);
  const physical_identity identity_b = physical_identity(2);
  const physical_identity identity_c = physical_identity(3);
  REQUIRE(collector.observe(
      1, make_object_definition_mutation(
             procedure, identity_a, identity_b)));
  REQUIRE(collector.begin_savepoint(2, 1) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.observe(
      2, make_object_definition_mutation(
             procedure, identity_b, identity_c)));
  REQUIRE(collector.finish_savepoint(2, 1, false) ==
          object_definition_dependency_error::none);

  std::array<object_definition_mutation, 4> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  const object_definition_mutation *retained = find_kind(
      std::span<const object_definition_mutation>(copied.data(), result.count),
      dependency_kind::procedure);
  REQUIRE(retained != nullptr);
  REQUIRE(retained->previous_physical_identity == identity_a);
  REQUIRE(retained->current_physical_identity == identity_b);
}

TEST_CASE("ambiguous physical history remains guard-only after rollback",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  object_definition_dependency_collector<8, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.begin_savepoint(2, 1) ==
          object_definition_dependency_error::none);
  const dependency_key procedure = object_key(dependency_kind::procedure, 101);
  REQUIRE(collector.observe(
      2, make_object_definition_mutation(
             procedure, physical_identity(1), physical_identity(2))));
  const auto ambiguous = collector.observe(
      2, make_object_definition_mutation(
             procedure, physical_identity(3), physical_identity(4)));
  REQUIRE(ambiguous.error ==
          object_definition_dependency_error::ambiguous_mutation);
  REQUIRE(ambiguous.observation ==
          object_definition_observation::database_definition_guard_only);
  REQUIRE(ambiguous.root_guard_required);
  REQUIRE(collector.guard_only());
  REQUIRE(collector.finish_savepoint(2, 1, false) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.transaction_dirty());

  std::array<object_definition_mutation, 4> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result.count == 1);
  REQUIRE(result.guard_only);
  REQUIRE(database_definition_guard_key_valid(copied[0].key));
}

TEST_CASE("specific capacity and output pressure fail closed to one guard",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  SECTION("specific-key pressure") {
    object_definition_dependency_collector<2, 4> collector;
    REQUIRE(collector.initialize(17, 1) ==
            object_definition_dependency_error::none);
    REQUIRE(collector.observe(
        1, make_object_definition_mutation(
               object_key(dependency_kind::procedure, 101))));
    REQUIRE(collector.observe(
        1, make_object_definition_mutation(
               object_key(dependency_kind::type, 102, 1247))));
    const auto overflow = collector.observe(
        1, make_object_definition_mutation(
               object_key(dependency_kind::operator_object, 103, 2617)));
    REQUIRE(overflow.error ==
            object_definition_dependency_error::capacity_exceeded);
    REQUIRE(collector.guard_only());

    std::array<object_definition_mutation, 3> copied{};
    const auto result = collector.copy_committed_dependencies(copied);
    REQUIRE(result.count == 1);
    REQUIRE(result.guard_only);
    REQUIRE(database_definition_guard_key_valid(copied[0].key));
  }

  SECTION("destination pressure") {
    object_definition_dependency_collector<4, 4> collector;
    REQUIRE(collector.initialize(17, 1) ==
            object_definition_dependency_error::none);
    REQUIRE(collector.observe(
        1, make_object_definition_mutation(
               object_key(dependency_kind::procedure, 101))));
    REQUIRE(collector.observe(
        1, make_object_definition_mutation(
               object_key(dependency_kind::type, 102, 1247))));
    std::array<object_definition_mutation, 2> copied{};
    const auto result = collector.copy_committed_dependencies(copied);
    REQUIRE(result.error ==
            object_definition_dependency_error::destination_too_small);
    REQUIRE(result.count == 1);
    REQUIRE(result.required_count == 3);
    REQUIRE(result.guard_only);
    REQUIRE(collector.guard_only());
    REQUIRE(database_definition_guard_key_valid(copied[0].key));
  }
}

TEST_CASE("child capacity overflow keeps the root guard after rollback",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  object_definition_dependency_collector<2, 4> collector;
  REQUIRE(collector.initialize(17, 1) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.begin_savepoint(2, 1) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.observe(
      2, make_object_definition_mutation(
             object_key(dependency_kind::procedure, 101))));
  REQUIRE(collector.observe(
      2, make_object_definition_mutation(
             object_key(dependency_kind::type, 102, 1247))));
  const auto overflow = collector.observe(
      2, make_object_definition_mutation(
             object_key(dependency_kind::operator_object, 103, 2617)));
  REQUIRE(overflow.error ==
          object_definition_dependency_error::capacity_exceeded);
  REQUIRE(collector.finish_savepoint(2, 1, false) ==
          object_definition_dependency_error::none);
  REQUIRE(collector.guard_only());
  REQUIRE(collector.transaction_dirty());

  std::array<object_definition_mutation, 3> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result);
  REQUIRE(result.guard_only);
  REQUIRE(result.count == 1);
  REQUIRE(database_definition_guard_key_valid(copied[0].key));
}

TEST_CASE("definition capture reserves both database epoch references",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  object_definition_dependency_collector<> collector;
  REQUIRE(collector.initialize(17, 1) ==
          object_definition_dependency_error::none);
  for (std::uint32_t index = 0;
       index < k_maximum_object_definition_specific_dependencies; ++index) {
    REQUIRE(collector.observe(
        1, make_object_definition_mutation(
               object_key(dependency_kind::procedure, 1'000U + index))));
  }
  std::array<object_definition_mutation, 256> copied{};
  const auto result = collector.copy_committed_dependencies(copied);
  REQUIRE(result);
  REQUIRE_FALSE(result.guard_only);
  REQUIRE(result.count ==
          k_maximum_object_definition_specific_dependencies + 1U);
  REQUIRE(result.required_count == result.count);
  REQUIRE(std::any_of(
      copied.begin(), copied.end(),
      [](const object_definition_mutation &mutation) noexcept {
        return database_definition_guard_key_valid(mutation.key);
      }));
}

TEST_CASE("invalid keys and savepoint tracking failures retain the guard",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  SECTION("unsupported key") {
    object_definition_dependency_collector<4, 4> collector;
    REQUIRE(collector.initialize(17, 1) ==
            object_definition_dependency_error::none);
    const auto invalid = collector.observe(
        1, make_object_definition_mutation(
               object_key(dependency_kind::relation_content, 101)));
    REQUIRE(invalid.error ==
            object_definition_dependency_error::ambiguous_mutation);
    REQUIRE(invalid.root_guard_required);
    REQUIRE(invalid.guard_only);
  }

  SECTION("wrong database") {
    object_definition_dependency_collector<4, 4> collector;
    REQUIRE(collector.initialize(17, 1) ==
            object_definition_dependency_error::none);
    dependency_key wrong_database =
        object_key(dependency_kind::procedure, 101);
    wrong_database.database_oid = 18;
    const auto invalid = collector.observe(
        1, make_object_definition_mutation(wrong_database));
    REQUIRE(invalid.error ==
            object_definition_dependency_error::invalid_argument);
    REQUIRE(collector.guard_only());
  }

  SECTION("frame pressure") {
    object_definition_dependency_collector<4, 1> collector;
    REQUIRE(collector.initialize(17, 1) ==
            object_definition_dependency_error::none);
    REQUIRE(collector.begin_savepoint(2, 1) ==
            object_definition_dependency_error::capacity_exceeded);
    REQUIRE(collector.guard_only());
    REQUIRE(collector.transaction_dirty());
  }
}

TEST_CASE("reset removes the sticky guard and all transaction state",
          "[component:cache][layer:unit][feature:dependency_tracking]") {
  object_definition_dependency_collector<2, 2> collector;
  REQUIRE(collector.initialize(17, 1) ==
          object_definition_dependency_error::none);
  collector.force_database_definition_guard();
  REQUIRE(collector.guard_only());
  collector.reset();
  REQUIRE_FALSE(collector.guard_only());
  REQUIRE_FALSE(collector.transaction_dirty());
  REQUIRE(collector.observation_count() == 0);
  REQUIRE(collector.frame_count() == 0);
}

}  // namespace
