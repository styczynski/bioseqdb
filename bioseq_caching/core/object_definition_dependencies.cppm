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
    k_maximum_object_definition_specific_dependencies =
        k_maximum_precise_dirty_dependencies_per_backend - 2U;
inline constexpr std::size_t k_maximum_object_definition_frames = 64;

/** The net physical-identity effect represented by one mutation. */
enum class definition_physical_change : std::uint8_t {
  unchanged = 0,
  created = 1,
  replaced = 2,
  dropped = 3,
};

/** Stable outcome for one allocation-free collection operation. */
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

/** Whether a precise mutation changed the transaction-local set. */
enum class object_definition_observation : std::uint8_t {
  recorded = 0,
  updated,
  duplicate,
  database_definition_guard_only,
};

/** One object-definition key and its old/current physical identity. */
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

/** Result returned to a PostgreSQL hook before it calls the shared runtime. */
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

/** Canonical committed-set copy result; a returned prefix is authoritative. */
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

/** Build the mandatory per-database definition-epoch backstop key. */
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

/** Validate the exact shape of the database-definition guard. */
[[nodiscard]] constexpr bool database_definition_guard_key_valid(
    const dependency_key &key) noexcept {
  return key.database_oid != 0 && key.class_oid == 0 &&
         key.object_oid == key.database_oid && key.object_sub_id == 0 &&
         key.kind == dependency_kind::database_definition_epoch &&
         key.reserved == 0;
}

/** Identify the object/syscache kinds whose precise epochs are supported. */
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

/** Validate one precise object key before it enters transaction state. */
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

/** Derive the net old-to-current physical transition without ambiguity. */
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

/** Construct a canonical object mutation from fixed-width identities. */
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

/** Construct the exact PlanInvalItem/syscache hash dependency encoding. */
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

/** Validate reserved bytes, key identity, and physical-transition metadata. */
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
 * Fixed-capacity object-definition collector with nested savepoint semantics.
 *
 * `p_specific_capacity` excludes the mandatory database-definition guard and
 * leaves one owner slot for the database-write guard staged by the runtime,
 * so the default precise output remains within its 256-key owner budget.
 * Once `guard_only()` becomes true it stays true until `reset()`;
 * rolling back the savepoint that exposed ambiguity cannot weaken it.
 */
template <std::size_t p_specific_capacity =
              k_maximum_object_definition_specific_dependencies,
          std::size_t p_frame_capacity = k_maximum_object_definition_frames>
  requires(p_specific_capacity > 0 && p_frame_capacity > 0)
class object_definition_dependency_collector {
 public:
  using subtransaction_id = std::uint32_t;

  /** Initialize one top transaction and its preallocated guard identity. */
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

  /** Push a strictly nested savepoint frame without allocating. */
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
   * Record or compose one event in the current savepoint.
   *
   * The result always names the root guard after any classified event.  A
   * PostgreSQL adapter must retain that guard in the top-transaction frame,
   * not merely in the current child frame.
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

  /** Release or roll back the current savepoint using strict LIFO identity. */
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

  /** Force the per-database guard after an adapter-side ambiguity. */
  void force_database_definition_guard() noexcept { enter_guard_only(); }

  /**
   * Copy the top-transaction dependency set in canonical key order.
   *
   * If the destination cannot hold the precise set, the collector becomes
   * guard-only and emits that one authoritative guard whenever space permits.
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

  /** Remove every top-transaction and savepoint observation. */
  void reset() noexcept { *this = {}; }

  /** Return the configured database-definition guard key. */
  [[nodiscard]] dependency_key guard_key() const noexcept {
    return database_definition_guard_key(database_oid_);
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

  /** Return one canonical guard mutation for commit iteration. */
  [[nodiscard]] object_definition_mutation guard_mutation() const noexcept {
    return make_object_definition_mutation(guard_key());
  }

  /** Find an exact key owned by one frame. */
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

  /** Find an exact key in any strict ancestor frame. */
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

  /** Return the first inactive fixed-capacity observation slot. */
  [[nodiscard]] observation *first_free_observation() noexcept {
    for (observation &observation : observations_) {
      if (!observation.active) {
        return &observation;
      }
    }
    return nullptr;
  }

  /** Compose sequential physical transitions for the same dependency key. */
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

  /** Remove every precise mutation owned by one frame. */
  void remove_frame_observations(std::size_t owner_frame) noexcept {
    for (observation &observation : observations_) {
      if (observation.active && observation.owner_frame == owner_frame) {
        observation = {};
        --observation_count_;
      }
    }
  }

  /** Merge one released child into its parent, composing exact keys. */
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
