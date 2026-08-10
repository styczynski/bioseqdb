/**
 * @file bioseq_caching/core/write_observation.cppm
 * @brief Fixed transaction/subtransaction write-observation ledger.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.cache.core:write_observation;

export namespace bioseq::cache {

enum class WriteObservationResultV1 : std::uint8_t {
  Recorded = 0,
  Duplicate = 1,
  CoarseFallback = 2,
};

enum class WriteSubtransactionResultV1 : std::uint8_t {
  NoFrame = 0,
  Committed = 1,
  Aborted = 2,
  CoarseFallback = 3,
};

/**
 * A bounded allocation-free transaction ledger.
 *
 * Keys are owned by their current subtransaction.  A subtransaction commit
 * retags its unique keys to the parent; abort removes only the child's keys.
 * Capacity failure makes coarse mode sticky until top-level cleanup, even if
 * the subtransaction which observed the failure later aborts.  This permits
 * false invalidation but never a missed committed write.
 */
template <class Key, std::size_t KeyCapacity,
          std::size_t FrameCapacity = 64>
  requires(std::is_trivially_copyable_v<Key> && KeyCapacity > 0 &&
           FrameCapacity > 0)
class TransactionWriteLedgerV1 {
public:
  using SubtransactionId = std::uint32_t;

  [[nodiscard]] WriteObservationResultV1 observe(
      SubtransactionId subtransaction_id, const Key &key) noexcept {
    if (sticky_coarse_) {
      return WriteObservationResultV1::CoarseFallback;
    }
    if (subtransaction_id == 0) {
      make_coarse();
      return WriteObservationResultV1::CoarseFallback;
    }
    Frame *frame = find_or_create_frame(subtransaction_id);
    if (frame == nullptr) {
      make_coarse();
      return WriteObservationResultV1::CoarseFallback;
    }
    frame->dirty = true;
    for (const Observation &observation : observations_) {
      if (observation.active && observation.owner == subtransaction_id &&
          observation.key == key) {
        return WriteObservationResultV1::Duplicate;
      }
    }
    for (Observation &observation : observations_) {
      if (!observation.active) {
        observation = Observation{
            .key = key,
            .owner = subtransaction_id,
            .active = true,
        };
        ++observation_count_;
        return WriteObservationResultV1::Recorded;
      }
    }
    make_coarse();
    return WriteObservationResultV1::CoarseFallback;
  }

  void observe_coarse(SubtransactionId subtransaction_id) noexcept {
    if (sticky_coarse_) {
      return;
    }
    if (subtransaction_id != 0) {
      if (Frame *frame = find_or_create_frame(subtransaction_id);
          frame != nullptr) {
        frame->dirty = true;
      }
    }
    make_coarse();
  }

  [[nodiscard]] WriteSubtransactionResultV1 finish_subtransaction(
      SubtransactionId child_id, SubtransactionId parent_id,
      bool committed) noexcept {
    Frame *child = find_frame(child_id);
    if (child == nullptr) {
      return WriteSubtransactionResultV1::NoFrame;
    }
    if (!committed) {
      remove_owned_observations(child_id);
      *child = {};
      return WriteSubtransactionResultV1::Aborted;
    }
    Frame *parent = parent_id == 0 ? nullptr : find_or_create_frame(parent_id);
    if (parent == nullptr) {
      remove_owned_observations(child_id);
      *child = {};
      make_coarse();
      return WriteSubtransactionResultV1::CoarseFallback;
    }
    parent->dirty = parent->dirty || child->dirty;
    for (Observation &candidate : observations_) {
      if (!candidate.active || candidate.owner != child_id) {
        continue;
      }
      bool duplicate = false;
      for (const Observation &existing : observations_) {
        if (&existing != &candidate && existing.active &&
            existing.owner == parent_id && existing.key == candidate.key) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        candidate = {};
        --observation_count_;
      } else {
        candidate.owner = parent_id;
      }
    }
    *child = {};
    return WriteSubtransactionResultV1::Committed;
  }

  [[nodiscard]] bool transaction_dirty() const noexcept {
    if (sticky_coarse_) {
      return true;
    }
    for (const Frame &frame : frames_) {
      if (frame.active && frame.dirty) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool coarse() const noexcept { return sticky_coarse_; }

  [[nodiscard]] bool touches(const Key &key) const noexcept {
    if (sticky_coarse_) {
      return true;
    }
    for (const Observation &observation : observations_) {
      if (observation.active && observation.key == key) {
        return true;
      }
    }
    return false;
  }

  /** Copy each distinct surviving key once, preserving bounded behavior. */
  [[nodiscard]] std::size_t copy_unique(std::span<Key> output) const noexcept {
    std::size_t copied = 0;
    for (const Observation &candidate : observations_) {
      if (!candidate.active) {
        continue;
      }
      bool duplicate = false;
      for (std::size_t index = 0; index < copied; ++index) {
        if (output[index] == candidate.key) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        continue;
      }
      if (copied >= output.size()) {
        return copied;
      }
      output[copied++] = candidate.key;
    }
    return copied;
  }

  [[nodiscard]] std::size_t observation_count() const noexcept {
    return observation_count_;
  }

  [[nodiscard]] std::size_t frame_count() const noexcept {
    std::size_t count = 0;
    for (const Frame &frame : frames_) {
      count += frame.active ? 1U : 0U;
    }
    return count;
  }

  void reset() noexcept { *this = {}; }

private:
  struct Frame {
    SubtransactionId id = 0;
    bool active = false;
    bool dirty = false;
  };

  struct Observation {
    Key key{};
    SubtransactionId owner = 0;
    bool active = false;
  };

  [[nodiscard]] Frame *find_frame(SubtransactionId id) noexcept {
    for (Frame &frame : frames_) {
      if (frame.active && frame.id == id) {
        return &frame;
      }
    }
    return nullptr;
  }

  [[nodiscard]] Frame *find_or_create_frame(SubtransactionId id) noexcept {
    if (Frame *frame = find_frame(id); frame != nullptr) {
      return frame;
    }
    for (Frame &frame : frames_) {
      if (!frame.active) {
        frame = Frame{.id = id, .active = true, .dirty = false};
        return &frame;
      }
    }
    return nullptr;
  }

  void remove_owned_observations(SubtransactionId owner) noexcept {
    for (Observation &observation : observations_) {
      if (observation.active && observation.owner == owner) {
        observation = {};
        --observation_count_;
      }
    }
  }

  void make_coarse() noexcept { sticky_coarse_ = true; }

  std::array<Frame, FrameCapacity> frames_{};
  std::array<Observation, KeyCapacity> observations_{};
  std::size_t observation_count_ = 0;
  bool sticky_coarse_ = false;
};

} // namespace bioseq::cache
