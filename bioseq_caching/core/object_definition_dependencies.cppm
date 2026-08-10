/**
 * @file bioseq_caching/core/object_definition_dependencies.cppm
 * @brief Bounded transaction-local object-definition dependency collection.
 *
 * The collector owns no PostgreSQL pointer or allocator.  Callers translate
 * object-access, syscache, and utility events into fixed-width mutations, then
 * retain the returned database-definition guard in the top transaction.  Any
 * ambiguity makes that guard sticky; precise object keys are only an
 * optimization and are never required for correctness.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.cache.core:object_definition_dependencies;

import :wire;

export namespace bioseq::cache {

inline constexpr std::size_t
    kMaximumObjectDefinitionSpecificDependenciesV1 =
        kMaxPreciseDirtyDependenciesPerBackend - 2U;
inline constexpr std::size_t kMaximumObjectDefinitionFramesV1 = 64;

/** The net physical-identity effect represented by one mutation. */
enum class DefinitionPhysicalChangeV1 : std::uint8_t {
  Unchanged = 0,
  Created = 1,
  Replaced = 2,
  Dropped = 3,
};

/** Stable outcome for one allocation-free collection operation. */
enum class ObjectDefinitionDependencyErrorV1 : std::uint8_t {
  None = 0,
  InvalidArgument,
  NotInitialized,
  InvalidSubtransaction,
  SavepointMismatch,
  CapacityExceeded,
  AmbiguousMutation,
  DestinationTooSmall,
};

/** Whether a precise mutation changed the transaction-local set. */
enum class ObjectDefinitionObservationV1 : std::uint8_t {
  Recorded = 0,
  Updated,
  Duplicate,
  DatabaseDefinitionGuardOnly,
};

/** One object-definition key and its old/current physical identity. */
struct ObjectDefinitionMutationV1 {
  DependencyKey key{};
  PhysicalIdentityV1 previous_physical_identity{};
  PhysicalIdentityV1 current_physical_identity{};
  DefinitionPhysicalChangeV1 physical_change =
      DefinitionPhysicalChangeV1::Unchanged;
  std::array<std::uint8_t, 7> reserved{};

  [[nodiscard]] bool operator==(
      const ObjectDefinitionMutationV1 &) const noexcept = default;
};

/** Result returned to a PostgreSQL hook before it calls the shared runtime. */
struct ObjectDefinitionObservationResultV1 {
  ObjectDefinitionDependencyErrorV1 error =
      ObjectDefinitionDependencyErrorV1::None;
  ObjectDefinitionObservationV1 observation =
      ObjectDefinitionObservationV1::Duplicate;
  ObjectDefinitionMutationV1 precise_mutation{};
  DependencyKey database_definition_guard{};
  bool root_guard_required = false;
  bool precise_runtime_update_required = false;
  bool guard_only = false;
  std::uint8_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == ObjectDefinitionDependencyErrorV1::None;
  }
};

/** Canonical committed-set copy result; a returned prefix is authoritative. */
struct ObjectDefinitionCopyResultV1 {
  ObjectDefinitionDependencyErrorV1 error =
      ObjectDefinitionDependencyErrorV1::None;
  std::size_t count = 0;
  std::size_t required_count = 0;
  bool guard_only = false;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == ObjectDefinitionDependencyErrorV1::None;
  }
};

/** Build the mandatory per-database definition-epoch backstop key. */
[[nodiscard]] constexpr DependencyKey database_definition_guard_key_v1(
    std::uint32_t database_oid) noexcept {
  return DependencyKey{
      .database_oid = database_oid,
      .class_oid = 0,
      .object_oid = database_oid,
      .object_sub_id = 0,
      .kind = DependencyKind::DatabaseDefinitionEpoch,
      .reserved = 0,
  };
}

/** Validate the exact shape of the database-definition guard. */
[[nodiscard]] constexpr bool database_definition_guard_key_valid_v1(
    const DependencyKey &key) noexcept {
  return key.database_oid != 0 && key.class_oid == 0 &&
         key.object_oid == key.database_oid && key.object_sub_id == 0 &&
         key.kind == DependencyKind::DatabaseDefinitionEpoch &&
         key.reserved == 0;
}

/** Identify the object/syscache kinds whose precise epochs are supported. */
[[nodiscard]] constexpr bool object_definition_dependency_kind_v1(
    DependencyKind kind) noexcept {
  switch (kind) {
    case DependencyKind::Procedure:
    case DependencyKind::Operator:
    case DependencyKind::Type:
    case DependencyKind::Cast:
    case DependencyKind::Collation:
    case DependencyKind::Policy:
    case DependencyKind::Role:
    case DependencyKind::SyscacheHash:
      return true;
    default:
      return false;
  }
}

/** Validate one precise object key before it enters transaction state. */
[[nodiscard]] constexpr bool object_definition_dependency_key_valid_v1(
    const DependencyKey &key) noexcept {
  if (key.database_oid == 0 || key.object_sub_id != 0 ||
      key.reserved != 0 ||
      !object_definition_dependency_kind_v1(key.kind)) {
    return false;
  }
  if (key.kind == DependencyKind::SyscacheHash) {
    return true;
  }
  return key.class_oid != 0 && key.object_oid != 0;
}

/** Return true only for the canonical empty physical identity. */
[[nodiscard]] constexpr bool physical_identity_empty_v1(
    const PhysicalIdentityV1 &identity) noexcept {
  return identity.kind == PhysicalIdentityKind::None &&
         identity.length == 0 && physical_identity_valid_v1(identity);
}

/** Derive the net old-to-current physical transition without ambiguity. */
[[nodiscard]] constexpr DefinitionPhysicalChangeV1
definition_physical_change_v1(
    const PhysicalIdentityV1 &previous,
    const PhysicalIdentityV1 &current) noexcept {
  if (previous == current) {
    return DefinitionPhysicalChangeV1::Unchanged;
  }
  if (physical_identity_empty_v1(previous)) {
    return DefinitionPhysicalChangeV1::Created;
  }
  if (physical_identity_empty_v1(current)) {
    return DefinitionPhysicalChangeV1::Dropped;
  }
  return DefinitionPhysicalChangeV1::Replaced;
}

/** Construct a canonical object mutation from fixed-width identities. */
[[nodiscard]] constexpr ObjectDefinitionMutationV1
make_object_definition_mutation_v1(
    DependencyKey key, PhysicalIdentityV1 previous = {},
    PhysicalIdentityV1 current = {}) noexcept {
  return ObjectDefinitionMutationV1{
      .key = key,
      .previous_physical_identity = previous,
      .current_physical_identity = current,
      .physical_change = definition_physical_change_v1(previous, current),
  };
}

/** Construct the exact PlanInvalItem/syscache hash dependency encoding. */
[[nodiscard]] constexpr ObjectDefinitionMutationV1
make_syscache_definition_mutation_v1(
    std::uint32_t database_oid, std::uint32_t cache_id,
    std::uint32_t hash_value) noexcept {
  return make_object_definition_mutation_v1(DependencyKey{
      .database_oid = database_oid,
      .class_oid = cache_id,
      .object_oid = hash_value,
      .object_sub_id = 0,
      .kind = DependencyKind::SyscacheHash,
      .reserved = 0,
  });
}

/** Validate reserved bytes, key identity, and physical-transition metadata. */
[[nodiscard]] constexpr bool object_definition_mutation_valid_v1(
    const ObjectDefinitionMutationV1 &mutation) noexcept {
  for (const std::uint8_t value : mutation.reserved) {
    if (value != 0) {
      return false;
    }
  }
  return object_definition_dependency_key_valid_v1(mutation.key) &&
         physical_identity_valid_v1(
             mutation.previous_physical_identity) &&
         physical_identity_valid_v1(
             mutation.current_physical_identity) &&
         mutation.physical_change == definition_physical_change_v1(
             mutation.previous_physical_identity,
             mutation.current_physical_identity);
}

/**
 * Fixed-capacity object-definition collector with nested savepoint semantics.
 *
 * `SpecificCapacity` excludes the mandatory database-definition guard and
 * leaves one owner slot for the database-write guard staged by the runtime,
 * so the default precise output remains within its 256-key owner budget.
 * Once `guard_only()` becomes true it stays true until `reset()`;
 * rolling back the savepoint that exposed ambiguity cannot weaken it.
 */
template <std::size_t SpecificCapacity =
              kMaximumObjectDefinitionSpecificDependenciesV1,
          std::size_t FrameCapacity = kMaximumObjectDefinitionFramesV1>
  requires(SpecificCapacity > 0 && FrameCapacity > 0)
class ObjectDefinitionDependencyCollectorV1 {
 public:
  using SubtransactionId = std::uint32_t;

  /** Initialize one top transaction and its preallocated guard identity. */
  [[nodiscard]] ObjectDefinitionDependencyErrorV1 initialize(
      std::uint32_t database_oid,
      SubtransactionId top_subtransaction_id) noexcept {
    reset();
    if (database_oid == 0 || top_subtransaction_id == 0) {
      return ObjectDefinitionDependencyErrorV1::InvalidArgument;
    }
    database_oid_ = database_oid;
    frames_[0] = Frame{
        .id = top_subtransaction_id,
        .parent = 0,
        .active = true,
        .dirty = false,
    };
    frame_count_ = 1;
    initialized_ = true;
    return ObjectDefinitionDependencyErrorV1::None;
  }

  /** Push a strictly nested savepoint frame without allocating. */
  [[nodiscard]] ObjectDefinitionDependencyErrorV1 begin_savepoint(
      SubtransactionId child, SubtransactionId parent) noexcept {
    if (!initialized_) {
      return ObjectDefinitionDependencyErrorV1::NotInitialized;
    }
    if (child == 0 || parent == 0 || child == parent ||
        frame_count_ == 0 || frames_[frame_count_ - 1U].id != parent) {
      enter_guard_only();
      return ObjectDefinitionDependencyErrorV1::SavepointMismatch;
    }
    if (frame_count_ >= FrameCapacity) {
      enter_guard_only();
      return ObjectDefinitionDependencyErrorV1::CapacityExceeded;
    }
    frames_[frame_count_++] = Frame{
        .id = child,
        .parent = parent,
        .active = true,
        .dirty = false,
    };
    return ObjectDefinitionDependencyErrorV1::None;
  }

  /**
   * Record or compose one event in the current savepoint.
   *
   * The result always names the root guard after any classified event.  A
   * PostgreSQL adapter must retain that guard in the top-transaction frame,
   * not merely in the current child frame.
   */
  [[nodiscard]] ObjectDefinitionObservationResultV1 observe(
      SubtransactionId subtransaction_id,
      const ObjectDefinitionMutationV1 &mutation) noexcept {
    ObjectDefinitionObservationResultV1 result{
        .database_definition_guard = guard_key(),
    };
    if (!initialized_) {
      result.error = ObjectDefinitionDependencyErrorV1::NotInitialized;
      result.observation =
          ObjectDefinitionObservationV1::DatabaseDefinitionGuardOnly;
      result.guard_only = true;
      return result;
    }
    if (subtransaction_id == 0 || frame_count_ == 0 ||
        frames_[frame_count_ - 1U].id != subtransaction_id) {
      enter_guard_only();
      result.error =
          ObjectDefinitionDependencyErrorV1::InvalidSubtransaction;
      result.observation =
          ObjectDefinitionObservationV1::DatabaseDefinitionGuardOnly;
      result.database_definition_guard = guard_key();
      result.root_guard_required = true;
      result.guard_only = true;
      return result;
    }

    transaction_dirty_ = true;
    frames_[frame_count_ - 1U].dirty = true;
    result.root_guard_required = true;
    if (guard_only_ ||
        mutation.key.database_oid != database_oid_ ||
        !object_definition_mutation_valid_v1(mutation)) {
      if (!guard_only_) {
        enter_guard_only();
        result.error = mutation.key.database_oid != database_oid_
            ? ObjectDefinitionDependencyErrorV1::InvalidArgument
            : ObjectDefinitionDependencyErrorV1::AmbiguousMutation;
      }
      result.observation =
          ObjectDefinitionObservationV1::DatabaseDefinitionGuardOnly;
      result.guard_only = true;
      return result;
    }

    const std::size_t current_frame = frame_count_ - 1U;
    if (Observation *existing = find_in_frame(
            mutation.key, current_frame);
        existing != nullptr) {
      if (existing->mutation == mutation) {
        result.observation = ObjectDefinitionObservationV1::Duplicate;
        result.precise_mutation = existing->mutation;
        return result;
      }
      ObjectDefinitionMutationV1 composed{};
      if (!compose_mutations(existing->mutation, mutation, &composed)) {
        enter_guard_only();
        result.error =
            ObjectDefinitionDependencyErrorV1::AmbiguousMutation;
        result.observation =
            ObjectDefinitionObservationV1::DatabaseDefinitionGuardOnly;
        result.guard_only = true;
        return result;
      }
      existing->mutation = composed;
      result.observation = ObjectDefinitionObservationV1::Updated;
      result.precise_mutation = composed;
      result.precise_runtime_update_required = true;
      return result;
    }

    if (const Observation *ancestor = find_in_ancestors(
            mutation.key, current_frame);
        ancestor != nullptr && ancestor->mutation == mutation) {
      result.observation = ObjectDefinitionObservationV1::Duplicate;
      result.precise_mutation = ancestor->mutation;
      return result;
    }

    Observation *slot = first_free_observation();
    if (slot == nullptr) {
      enter_guard_only();
      result.error = ObjectDefinitionDependencyErrorV1::CapacityExceeded;
      result.observation =
          ObjectDefinitionObservationV1::DatabaseDefinitionGuardOnly;
      result.guard_only = true;
      return result;
    }
    *slot = Observation{
        .mutation = mutation,
        .owner_frame = current_frame,
        .active = true,
    };
    ++observation_count_;
    result.observation = ObjectDefinitionObservationV1::Recorded;
    result.precise_mutation = mutation;
    result.precise_runtime_update_required = true;
    return result;
  }

  /** Release or roll back the current savepoint using strict LIFO identity. */
  [[nodiscard]] ObjectDefinitionDependencyErrorV1 finish_savepoint(
      SubtransactionId child, SubtransactionId parent,
      bool committed) noexcept {
    if (!initialized_) {
      return ObjectDefinitionDependencyErrorV1::NotInitialized;
    }
    if (frame_count_ <= 1 || child == 0 || parent == 0 ||
        frames_[frame_count_ - 1U].id != child ||
        frames_[frame_count_ - 1U].parent != parent ||
        frames_[frame_count_ - 2U].id != parent) {
      enter_guard_only();
      return ObjectDefinitionDependencyErrorV1::SavepointMismatch;
    }

    const std::size_t child_frame = frame_count_ - 1U;
    const std::size_t parent_frame = child_frame - 1U;
    const bool child_dirty = frames_[child_frame].dirty;
    ObjectDefinitionDependencyErrorV1 error =
        ObjectDefinitionDependencyErrorV1::None;
    if (committed && !guard_only_) {
      error = merge_frame(child_frame, parent_frame);
      frames_[parent_frame].dirty =
          frames_[parent_frame].dirty || child_dirty;
    } else {
      remove_frame_observations(child_frame);
    }
    frames_[child_frame] = {};
    --frame_count_;
    recompute_transaction_dirty();
    return error;
  }

  /** Force the per-database guard after an adapter-side ambiguity. */
  void force_database_definition_guard() noexcept { enter_guard_only(); }

  /**
   * Copy the top-transaction dependency set in canonical key order.
   *
   * If the destination cannot hold the precise set, the collector becomes
   * guard-only and emits that one authoritative guard whenever space permits.
   */
  [[nodiscard]] ObjectDefinitionCopyResultV1 copy_committed_dependencies(
      std::span<ObjectDefinitionMutationV1> output) noexcept {
    if (!initialized_) {
      return {
          .error = ObjectDefinitionDependencyErrorV1::NotInitialized,
      };
    }
    if (!transaction_dirty_) {
      return {};
    }
    if (frame_count_ != 1) {
      enter_guard_only();
    }

    const std::size_t required = guard_only_
        ? 1U
        : observation_count_ + 1U;
    if (output.empty()) {
      enter_guard_only();
      return {
          .error =
              ObjectDefinitionDependencyErrorV1::DestinationTooSmall,
          .count = 0,
          .required_count = 1,
          .guard_only = true,
      };
    }
    if (output.size() < required) {
      enter_guard_only();
      output[0] = guard_mutation();
      return {
          .error =
              ObjectDefinitionDependencyErrorV1::DestinationTooSmall,
          .count = 1,
          .required_count = required,
          .guard_only = true,
      };
    }

    output[0] = guard_mutation();
    if (guard_only_) {
      return {
          .count = 1,
          .required_count = 1,
          .guard_only = true,
      };
    }
    std::size_t copied = 1;
    for (const Observation &observation : observations_) {
      if (observation.active) {
        output[copied++] = observation.mutation;
      }
    }
    for (std::size_t index = 1; index < copied; ++index) {
      ObjectDefinitionMutationV1 candidate = output[index];
      std::size_t insertion = index;
      while (insertion > 0 && dependency_key_less_v1(
                 candidate.key, output[insertion - 1U].key)) {
        output[insertion] = output[insertion - 1U];
        --insertion;
      }
      output[insertion] = candidate;
    }
    return {
        .count = copied,
        .required_count = copied,
        .guard_only = false,
    };
  }

  /** Remove every top-transaction and savepoint observation. */
  void reset() noexcept { *this = {}; }

  /** Return the configured database-definition guard key. */
  [[nodiscard]] DependencyKey guard_key() const noexcept {
    return database_definition_guard_key_v1(database_oid_);
  }

  /** Report whether any surviving event or sticky ambiguity exists. */
  [[nodiscard]] bool transaction_dirty() const noexcept {
    return transaction_dirty_;
  }

  /** Report whether precise object epochs must be ignored. */
  [[nodiscard]] bool guard_only() const noexcept { return guard_only_; }

  /** Return the number of retained precise mutations. */
  [[nodiscard]] std::size_t observation_count() const noexcept {
    return observation_count_;
  }

  /** Return the number of tracked nested transaction frames. */
  [[nodiscard]] std::size_t frame_count() const noexcept {
    return frame_count_;
  }

 private:
  struct Frame {
    SubtransactionId id = 0;
    SubtransactionId parent = 0;
    bool active = false;
    bool dirty = false;
  };

  struct Observation {
    ObjectDefinitionMutationV1 mutation{};
    std::size_t owner_frame = 0;
    bool active = false;
  };

  /** Return one canonical guard mutation for commit iteration. */
  [[nodiscard]] ObjectDefinitionMutationV1 guard_mutation() const noexcept {
    return make_object_definition_mutation_v1(guard_key());
  }

  /** Find an exact key owned by one frame. */
  [[nodiscard]] Observation *find_in_frame(
      const DependencyKey &key, std::size_t owner_frame) noexcept {
    for (Observation &observation : observations_) {
      if (observation.active &&
          observation.owner_frame == owner_frame &&
          observation.mutation.key == key) {
        return &observation;
      }
    }
    return nullptr;
  }

  /** Find an exact key in any strict ancestor frame. */
  [[nodiscard]] const Observation *find_in_ancestors(
      const DependencyKey &key, std::size_t child_frame) const noexcept {
    for (const Observation &observation : observations_) {
      if (observation.active &&
          observation.owner_frame < child_frame &&
          observation.mutation.key == key) {
        return &observation;
      }
    }
    return nullptr;
  }

  /** Return the first inactive fixed-capacity observation slot. */
  [[nodiscard]] Observation *first_free_observation() noexcept {
    for (Observation &observation : observations_) {
      if (!observation.active) {
        return &observation;
      }
    }
    return nullptr;
  }

  /** Compose sequential physical transitions for the same dependency key. */
  [[nodiscard]] static bool compose_mutations(
      const ObjectDefinitionMutationV1 &earlier,
      const ObjectDefinitionMutationV1 &later,
      ObjectDefinitionMutationV1 *output) noexcept {
    if (output == nullptr || earlier.key != later.key ||
        earlier.current_physical_identity !=
            later.previous_physical_identity) {
      return false;
    }
    *output = make_object_definition_mutation_v1(
        earlier.key, earlier.previous_physical_identity,
        later.current_physical_identity);
    return true;
  }

  /** Remove every precise mutation owned by one frame. */
  void remove_frame_observations(std::size_t owner_frame) noexcept {
    for (Observation &observation : observations_) {
      if (observation.active && observation.owner_frame == owner_frame) {
        observation = {};
        --observation_count_;
      }
    }
  }

  /** Merge one released child into its parent, composing exact keys. */
  [[nodiscard]] ObjectDefinitionDependencyErrorV1 merge_frame(
      std::size_t child_frame, std::size_t parent_frame) noexcept {
    for (Observation &child : observations_) {
      if (!child.active || child.owner_frame != child_frame) {
        continue;
      }
      Observation *parent = find_in_frame(
          child.mutation.key, parent_frame);
      if (parent == nullptr) {
        child.owner_frame = parent_frame;
        continue;
      }
      if (parent->mutation == child.mutation) {
        child = {};
        --observation_count_;
        continue;
      }
      ObjectDefinitionMutationV1 composed{};
      if (!compose_mutations(
              parent->mutation, child.mutation, &composed)) {
        enter_guard_only();
        return ObjectDefinitionDependencyErrorV1::AmbiguousMutation;
      }
      parent->mutation = composed;
      child = {};
      --observation_count_;
    }
    return ObjectDefinitionDependencyErrorV1::None;
  }

  /** Recompute ordinary dirtiness after a non-sticky savepoint rollback. */
  void recompute_transaction_dirty() noexcept {
    if (guard_only_) {
      transaction_dirty_ = true;
      return;
    }
    transaction_dirty_ = false;
    for (std::size_t index = 0; index < frame_count_; ++index) {
      if (frames_[index].active && frames_[index].dirty) {
        transaction_dirty_ = true;
        return;
      }
    }
  }

  /** Discard precise state and retain the preallocated root guard promise. */
  void enter_guard_only() noexcept {
    if (!initialized_) {
      return;
    }
    guard_only_ = true;
    transaction_dirty_ = true;
    observation_count_ = 0;
    for (Observation &observation : observations_) {
      observation = {};
    }
  }

  std::array<Frame, FrameCapacity> frames_{};
  std::array<Observation, SpecificCapacity> observations_{};
  std::size_t frame_count_ = 0;
  std::size_t observation_count_ = 0;
  std::uint32_t database_oid_ = 0;
  bool initialized_ = false;
  bool transaction_dirty_ = false;
  bool guard_only_ = false;
};

static_assert(std::is_trivially_copyable_v<ObjectDefinitionMutationV1>);
static_assert(std::is_standard_layout_v<ObjectDefinitionMutationV1>);

}  // namespace bioseq::cache
