/**
 * @file bioseq_caching/core/object_definition_dependencies.cppm
 * @brief Tracks definition changes within a transaction and its savepoints.
 *
 * PostgreSQL hooks supply object changes as plain values. The collector
 * keeps individual dependencies while they fit and are unambiguous
 * Otherwise it requires a database-wide definition check until reset,
 * even if the savepoint that caused the problem is rolled back.
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
    k_maximum_object_definition_specific_dependencies =
        k_maximum_precise_dirty_dependencies_per_backend - 2U;
inline constexpr std::size_t k_maximum_object_definition_frames = 64;

/** How a mutation changes the object's physical identity. */
enum class definition_physical_change : std::uint8_t {
  unchanged = 0,
  created = 1,
  replaced = 2,
  dropped = 3,
};

/** Errors reported while collecting definition dependencies. */
enum class object_definition_dependency_error : std::uint8_t {
  none = 0,
  invalid_argument,
  not_initialized,
  invalid_subtransaction,
  savepoint_mismatch,
  capacity_exceeded,
  ambiguous_mutation,
  destination_too_small,
};

/** How recording a mutation affected the tracked dependencies. */
enum class object_definition_observation : std::uint8_t {
  recorded = 0,
  updated,
  duplicate,
  database_definition_guard_only,
};

/** An object dependency with its physical identity before  and after a change. */
struct object_definition_mutation {
  dependency_key key{};
  physical_identity previous_physical_identity{};
  physical_identity current_physical_identity{};
  definition_physical_change physical_change =
      definition_physical_change::unchanged;
  std::array<std::uint8_t, 7> reserved{};

  [[nodiscard]] bool operator==(
      const object_definition_mutation &) const noexcept = default;
};

/** Tells a PostgreSQL hook which database-wide and object-specific dependencies to update. */
struct object_definition_observation_result {
  object_definition_dependency_error error =
      object_definition_dependency_error::none;
  object_definition_observation observation =
      object_definition_observation::duplicate;
  object_definition_mutation precise_mutation{};
  dependency_key database_definition_guard{};
  bool root_guard_required = false;
  bool precise_runtime_update_required = false;
  bool guard_only = false;
  std::uint8_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == object_definition_dependency_error::none;
  }
};

/** Reports how many dependencies were copibd and whether collection fell back to a database-wide check. */
struct object_definition_copy_result {
  object_definition_dependency_error error =
      object_definition_dependency_error::none;
  std::size_t count = 0;
  std::size_t required_count = 0;
  bool guard_only = false;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == object_definition_dependency_error::none;
  }
};

/** Builds the key for tracking definition changes across the whole database. */
[[nodiscard]] constexpr dependency_key database_definition_guard_key(
    std::uint32_t database_oid) noexcept {
  return dependency_key{
      .database_oid = database_oid,
      .class_oid = 0,
      .object_oid = database_oid,
      .object_sub_id = 0,
      .kind = dependency_kind::database_definition_epoch,
      .reserved = 0,
  };
}

/** Checks the fields of a database-wide definition dependency key. */
[[nodiscard]] constexpr bool database_definition_guard_key_valid(
    const dependency_key &key) noexcept {
  return key.database_oid != 0 && key.class_oid == 0 &&
         key.object_oid == key.database_oid && key.object_sub_id == 0 &&
         key.kind == dependency_kind::database_definition_epoch &&
         key.reserved == 0;
}

/** Reports which catalog-object kinds support individual definition tracking. */
[[nodiscard]] constexpr bool object_definition_dependency_kind(
    dependency_kind kind) noexcept {
  switch (kind) {
    case dependency_kind::procedure:
    case dependency_kind::operator_object:
    case dependency_kind::type:
    case dependency_kind::cast:
    case dependency_kind::collation:
    case dependency_kind::policy:
    case dependency_kind::role:
    case dependency_kind::syscache_hash:
      return true;
    default:
      return false;
  }
}

/** Checks an individual object or syscache dependency key. */
[[nodiscard]] constexpr bool object_definition_dependency_key_valid(
    const dependency_key &key) noexcept {
  if (key.database_oid == 0 || key.object_sub_id != 0 ||
      key.reserved != 0 ||
      !object_definition_dependency_kind(key.kind)) {
    return false;
  }
  if (key.kind == dependency_kind::syscache_hash) {
    return true;
  }
  return key.class_oid != 0 && key.object_oid != 0;
}

/** Return true only for the canonical empty physical identity. */
[[nodiscard]] constexpr bool physical_identity_empty(
    const physical_identity &identity) noexcept {
  return identity.kind == physical_identity_kind::none &&
         identity.length == 0 && physical_identity_valid(identity);
}

/** Classifies the change from the previous physical identity to the current  one. */
[[nodiscard]] constexpr definition_physical_change
classify_definition_physical_change(
    const physical_identity &previous,
    const physical_identity &current) noexcept {
  if (previous == current) {
    return definition_physical_change::unchanged;
  }
  if (physical_identity_empty(previous)) {
    return definition_physical_change::created;
  }
  if (physical_identity_empty(current)) {
    return definition_physical_change::dropped;
  }
  return definition_physical_change::replaced;
}

/** Builds a mutation and classifies its physical-identity change. */
[[nodiscard]] constexpr object_definition_mutation
make_object_definition_mutation(
    dependency_key key, physical_identity previous = {},
    physical_identity current = {}) noexcept {
  return object_definition_mutation{
      .key = key,
      .previous_physical_identity = previous,
      .current_physical_identity = current,
      .physical_change = classify_definition_physical_change(previous, current),
  };
}

/** Builds a dependency from a database ID, syscache ID and invalidation hash. */
[[nodiscard]] constexpr object_definition_mutation
make_syscache_definition_mutation(
    std::uint32_t database_oid, std::uint32_t cache_id,
    std::uint32_t hash_value) noexcept {
  return make_object_definition_mutation(dependency_key{
      .database_oid = database_oid,
      .class_oid = cache_id,
      .object_oid = hash_value,
      .object_sub_id = 0,
      .kind = dependency_kind::syscache_hash,
      .reserved = 0,
  });
}

/** Checks the dependency key and physical identities, including the recorded change type. */
[[nodiscard]] constexpr bool object_definition_mutation_valid(
    const object_definition_mutation &mutation) noexcept {
  for (const std::uint8_t value : mutation.reserved) {
    if (value != 0) {
      return false;
    }
  }
  return object_definition_dependency_key_valid(mutation.key) &&
         physical_identity_valid(
             mutation.previous_physical_identity) &&
         physical_identity_valid(
             mutation.current_physical_identity) &&
         mutation.physical_change == classify_definition_physical_change(
             mutation.previous_physical_identity,
             mutation.current_physical_identity);
}

/**
 * Collects definition changes in fixed arrays, with nested savepoints.
 * The default capacity leaves room for database-wide definition and
 * write dependencies within the runtime's dependency limit.
 *
 * Once individual tracking becomes unsafe, guard_only() stays true
 * until reset(). Rolling back a savepoint does not clear that requirement
 */
template <std::size_t p_specific_capacity =
              k_maximum_object_definition_specific_dependencies,
          std::size_t p_frame_capacity = k_maximum_object_definition_frames>
  requires(p_specific_capacity > 0 && p_frame_capacity > 0)
class object_definition_dependency_collector {
 public:
  using subtransaction_id = std::uint32_t;

  /** Starts tracking a top-level transaction, clearing any, previous  state. */
  [[nodiscard]] object_definition_dependency_error initialize(
      std::uint32_t database_oid,
      subtransaction_id top_subtransaction_id) noexcept {
    reset();
    if (database_oid == 0 || top_subtransaction_id == 0) {
      return object_definition_dependency_error::invalid_argument;
    }
    database_oid_ = database_oid;
    frames_[0] = frame{
        .id = top_subtransaction_id,
        .parent = 0,
        .active = true,
        .dirty = false,
    };
    frame_count_ = 1;
    initialized_ = true;
    return object_definition_dependency_error::none;
  }

  /** Adds a child savepoint under the current frame, using the fixed frame array. */
  [[nodiscard]] object_definition_dependency_error begin_savepoint(
      subtransaction_id child, subtransaction_id parent) noexcept {
    if (!initialized_) {
      return object_definition_dependency_error::not_initialized;
    }
    if (child == 0 || parent == 0 || child == parent ||
        frame_count_ == 0 || frames_[frame_count_ - 1U].id != parent) {
      enter_guard_only();
      return object_definition_dependency_error::savepoint_mismatch;
    }
    if (frame_count_ >= p_frame_capacity) {
      enter_guard_only();
      return object_definition_dependency_error::capacity_exceeded;
    }
    frames_[frame_count_++] = frame{
        .id = child,
        .parent = parent,
        .active = true,
        .dirty = false,
    };
    return object_definition_dependency_error::none;
  }

  /**
   * Records a definition change in the current savepoint, combining
   * successive changes to the same objent. The PostgreSQL adapter must
   * keep the returned database-wide definition dependency in the top-level
   * transaction so a savepoint rollback cannot discard it.
   */
  [[nodiscard]] object_definition_observation_result observe(
      subtransaction_id current_subtransaction_id,
      const object_definition_mutation &mutation) noexcept {
    object_definition_observation_result result{
        .database_definition_guard = guard_key(),
    };
    if (!initialized_) {
      result.error = object_definition_dependency_error::not_initialized;
      result.observation =
          object_definition_observation::database_definition_guard_only;
      result.guard_only = true;
      return result;
    }
    if (current_subtransaction_id == 0 || frame_count_ == 0 ||
        frames_[frame_count_ - 1U].id != current_subtransaction_id) {
      enter_guard_only();
      result.error =
          object_definition_dependency_error::invalid_subtransaction;
      result.observation =
          object_definition_observation::database_definition_guard_only;
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
        !object_definition_mutation_valid(mutation)) {
      if (!guard_only_) {
        enter_guard_only();
        result.error = mutation.key.database_oid != database_oid_
            ? object_definition_dependency_error::invalid_argument
            : object_definition_dependency_error::ambiguous_mutation;
      }
      result.observation =
          object_definition_observation::database_definition_guard_only;
      result.guard_only = true;
      return result;
    }

    const std::size_t current_frame = frame_count_ - 1U;
    if (observation *existing = find_in_frame(
            mutation.key, current_frame);
        existing != nullptr) {
      if (existing->mutation == mutation) {
        result.observation = object_definition_observation::duplicate;
        result.precise_mutation = existing->mutation;
        return result;
      }
      object_definition_mutation composed{};
      if (!compose_mutations(existing->mutation, mutation, &composed)) {
        enter_guard_only();
        result.error =
            object_definition_dependency_error::ambiguous_mutation;
        result.observation =
            object_definition_observation::database_definition_guard_only;
        result.guard_only = true;
        return result;
      }
      existing->mutation = composed;
      result.observation = object_definition_observation::updated;
      result.precise_mutation = composed;
      result.precise_runtime_update_required = true;
      return result;
    }

    if (const observation *ancestor = find_in_ancestors(
            mutation.key, current_frame);
        ancestor != nullptr && ancestor->mutation == mutation) {
      result.observation = object_definition_observation::duplicate;
      result.precise_mutation = ancestor->mutation;
      return result;
    }

    observation *slot = first_free_observation();
    if (slot == nullptr) {
      enter_guard_only();
      result.error = object_definition_dependency_error::capacity_exceeded;
      result.observation =
          object_definition_observation::database_definition_guard_only;
      result.guard_only = true;
      return result;
    }
    *slot = observation{
        .mutation = mutation,
        .owner_frame = current_frame,
        .active = true,
    };
    ++observation_count_;
    result.observation = object_definition_observation::recorded;
    result.precise_mutation = mutation;
    result.precise_runtime_update_required = true;
    return result;
  }

  /** Releases or rolls back the current savepoint. Its child and parent IDs must match the top two frames. */
  [[nodiscard]] object_definition_dependency_error finish_savepoint(
      subtransaction_id child, subtransaction_id parent,
      bool committed) noexcept {
    if (!initialized_) {
      return object_definition_dependency_error::not_initialized;
    }
    if (frame_count_ <= 1 || child == 0 || parent == 0 ||
        frames_[frame_count_ - 1U].id != child ||
        frames_[frame_count_ - 1U].parent != parent ||
        frames_[frame_count_ - 2U].id != parent) {
      enter_guard_only();
      return object_definition_dependency_error::savepoint_mismatch;
    }

    const std::size_t child_frame = frame_count_ - 1U;
    const std::size_t parent_frame = child_frame - 1U;
    const bool child_dirty = frames_[child_frame].dirty;
    object_definition_dependency_error error =
        object_definition_dependency_error::none;
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

  /** Requires a database-wide  definition check after the adapter reports an uncertain change. */
  void force_database_definition_guard() noexcept { enter_guard_only(); }

  /**
   * Copies dependencies for the top-level transaction in sorted key
   * order. If the output is too small, the collector switches to the
   * database-wide definition dependency and writes it if space allows.
   */
  [[nodiscard]] object_definition_copy_result copy_committed_dependencies(
      std::span<object_definition_mutation> output) noexcept {
    if (!initialized_) {
      return {
          .error = object_definition_dependency_error::not_initialized,
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
              object_definition_dependency_error::destination_too_small,
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
              object_definition_dependency_error::destination_too_small,
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
    for (const observation &observation : observations_) {
      if (observation.active) {
        output[copied++] = observation.mutation;
      }
    }
    for (std::size_t index = 1; index < copied; ++index) {
      object_definition_mutation candidate = output[index];
      std::size_t insertion = index;
      while (insertion > 0 && dependency_key_less(
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

  /** Clears all observations and returns the collector to its uninitialized state. */
  void reset() noexcept { *this = {}; }

  /** Returns the dependency key used to check definitions across the database. */
  [[nodiscard]] dependency_key guard_key() const noexcept {
    return database_definition_guard_key(database_oid_);
  }

  /** Reports whether a surviving change or an unresolved tracking problem requires invalidation. */
  [[nodiscard]] bool transaction_dirty() const noexcept {
    return transaction_dirty_;
  }

  /** Reports whether  only database-wide definition tracking remains. */
  [[nodiscard]] bool guard_only() const noexcept { return guard_only_; }

  /** Returns the number of saved object-specific changes. */
  [[nodiscard]] std::size_t observation_count() const noexcept {
    return observation_count_;
  }

  /** Returns the number of active transaction frames, including the top-level transaction */
  [[nodiscard]] std::size_t frame_count() const noexcept {
    return frame_count_;
  }

 private:
  struct frame {
    subtransaction_id id = 0;
    subtransaction_id parent = 0;
    bool active = false;
    bool dirty = false;
  };

  struct observation {
    object_definition_mutation mutation{};
    std::size_t owner_frame = 0;
    bool active = false;
  };

  /** Builds the database-wide efinition mutation for commit processing. */
  [[nodiscard]] object_definition_mutation guard_mutation() const noexcept {
    return make_object_definition_mutation(guard_key());
  }

  /** Finds a dependency in the specified transaction frame, or returns null. */
  [[nodiscard]] observation *find_in_frame(
      const dependency_key &key, std::size_t owner_frame) noexcept {
    for (observation &observation : observations_) {
      if (observation.active &&
          observation.owner_frame == owner_frame &&
          observation.mutation.key == key) {
        return &observation;
      }
    }
    return nullptr;
  }

  /** Finds a matching dependency in an earlier transaction frame, or returns null. */
  [[nodiscard]] const observation *find_in_ancestors(
      const dependency_key &key, std::size_t child_frame) const noexcept {
    for (const observation &observation : observations_) {
      if (observation.active &&
          observation.owner_frame < child_frame &&
          observation.mutation.key == key) {
        return &observation;
      }
    }
    return nullptr;
  }

  /** Returns the first unused observation slot, or null if the array is full. */
  [[nodiscard]] observation *first_free_observation() noexcept {
    for (observation &observation : observations_) {
      if (!observation.active) {
        return &observation;
      }
    }
    return nullptr;
  }

  /** Combines changes to the same object  when the later change starts from the earlier result. */
  [[nodiscard]] static bool compose_mutations(
      const object_definition_mutation &earlier,
      const object_definition_mutation &later,
      object_definition_mutation *output) noexcept {
    if (output == nullptr || earlier.key != later.key ||
        earlier.current_physical_identity !=
            later.previous_physical_identity) {
      return false;
    }
    *output = make_object_definition_mutation(
        earlier.key, earlier.previous_physical_identity,
        later.current_physical_identity);
    return true;
  }

  /** Removes the object-specific changes recorded in one transaction frame. */
  void remove_frame_observations(std::size_t owner_frame) noexcept {
    for (observation &observation : observations_) {
      if (observation.active && observation.owner_frame == owner_frame) {
        observation = {};
        --observation_count_;
      }
    }
  }

  /** Moves a released savepoint's changes into its parent, combining changes to the same object. */
  [[nodiscard]] object_definition_dependency_error merge_frame(
      std::size_t child_frame, std::size_t parent_frame) noexcept {
    for (observation &child : observations_) {
      if (!child.active || child.owner_frame != child_frame) {
        continue;
      }
      observation *parent = find_in_frame(
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
      object_definition_mutation composed{};
      if (!compose_mutations(
              parent->mutation, child.mutation, &composed)) {
        enter_guard_only();
        return object_definition_dependency_error::ambiguous_mutation;
      }
      parent->mutation = composed;
      child = {};
      --observation_count_;
    }
    return object_definition_dependency_error::none;
  }

  /** Rechecks surviving changes after, a savepoint ends, preserving any database-wide fallback. */
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

  /** Clears the per-object changes and marks the database definitions as changed until reset(). */
  void enter_guard_only() noexcept {
    if (!initialized_) {
      return;
    }
    guard_only_ = true;
    transaction_dirty_ = true;
    observation_count_ = 0;
    for (observation &observation : observations_) {
      observation = {};
    }
  }

  std::array<frame, p_frame_capacity> frames_{};
  std::array<observation, p_specific_capacity> observations_{};
  std::size_t frame_count_ = 0;
  std::size_t observation_count_ = 0;
  std::uint32_t database_oid_ = 0;
  bool initialized_ = false;
  bool transaction_dirty_ = false;
  bool guard_only_ = false;
};

static_assert(std::is_trivially_copyable_v<object_definition_mutation>);
static_assert(std::is_standard_layout_v<object_definition_mutation>);

}  // namespace bioseq::cache
