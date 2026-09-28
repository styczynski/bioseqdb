/**
 * @file bioseq_caching/core/write_observation.cppm
 * @brief Tracks keys written by a transaction, and its subtransactions.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.cache.core:write_observation;

export namespace bioseq::cache {

enum class write_observation_result : std::uint8_t {
    recorded = 0,
    duplicate = 1,
    coarse_fallback = 2,
};

enum class write_subtransaction_result : std::uint8_t {
    no_frame = 0,
    committed = 1,
    aborted = 2,
    coarse_fallback = 3,
};

/**
 * Tracks written keys in fixed storage.
 *
 * Committing a subtransaction transfers its keys to the parent. Aborting it
 * removes its keys. If either array fills, the ledger treats the transaction
 * as touching every key until reset, even if the subtransaction later aborts.
 * Callers use this mode to invalidate entries when the ledger cannot name each
 * changed key.
 */
template <class p_key_type, std::size_t p_key_capacity, std::size_t p_frame_capacity = 64>
    requires(std::is_trivially_copyable_v<p_key_type> && p_key_capacity > 0 &&
             p_frame_capacity > 0)
class transaction_write_ledger {
  public:
    using subtransaction_id = std::uint32_t;

    [[nodiscard]] write_observation_result observe(subtransaction_id transaction_id,
                                                   const p_key_type& key) noexcept {
        if (sticky_coarse_) {
            return write_observation_result::coarse_fallback;
        }
        if (transaction_id == 0) {
            make_coarse();
            return write_observation_result::coarse_fallback;
        }
        frame* transaction_frame = find_or_create_frame(transaction_id);
        if (transaction_frame == nullptr) {
            make_coarse();
            return write_observation_result::coarse_fallback;
        }
        transaction_frame->dirty = true;
        for (const observation& entry : observations_) {
            if (entry.active && entry.owner == transaction_id && entry.key == key) {
                return write_observation_result::duplicate;
            }
        }
        for (observation& entry : observations_) {
            if (!entry.active) {
                entry = observation{
                    .key = key,
                    .owner = transaction_id,
                    .active = true,
                };
                ++observation_count_;
                return write_observation_result::recorded;
            }
        }
        make_coarse();
        return write_observation_result::coarse_fallback;
    }

    void observe_coarse(subtransaction_id transaction_id) noexcept {
        if (sticky_coarse_) {
            return;
        }
        if (transaction_id != 0) {
            if (frame* transaction_frame = find_or_create_frame(transaction_id);
                transaction_frame != nullptr) {
                transaction_frame->dirty = true;
            }
        }
        make_coarse();
    }

    [[nodiscard]] write_subtransaction_result finish_subtransaction(subtransaction_id child_id,
                                                                    subtransaction_id parent_id,
                                                                    bool committed) noexcept {
        frame* child = find_frame(child_id);
        if (child == nullptr) {
            return write_subtransaction_result::no_frame;
        }
        if (!committed) {
            remove_owned_observations(child_id);
            *child = {};
            return write_subtransaction_result::aborted;
        }
        frame* parent = parent_id == 0 ? nullptr : find_or_create_frame(parent_id);
        if (parent == nullptr) {
            remove_owned_observations(child_id);
            *child = {};
            make_coarse();
            return write_subtransaction_result::coarse_fallback;
        }
        parent->dirty = parent->dirty || child->dirty;
        for (observation& candidate : observations_) {
            if (!candidate.active || candidate.owner != child_id) {
                continue;
            }
            bool duplicate = false;
            for (const observation& existing : observations_) {
                if (&existing != &candidate && existing.active && existing.owner == parent_id &&
                    existing.key == candidate.key) {
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
        return write_subtransaction_result::committed;
    }

    [[nodiscard]] bool transaction_dirty() const noexcept {
        if (sticky_coarse_) {
            return true;
        }
        for (const frame& transaction_frame : frames_) {
            if (transaction_frame.active && transaction_frame.dirty) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool coarse() const noexcept {
        return sticky_coarse_;
    }

    [[nodiscard]] bool touches(const p_key_type& key) const noexcept {
        if (sticky_coarse_) {
            return true;
        }
        for (const observation& entry : observations_) {
            if (entry.active && entry.key == key) {
                return true;
            }
        }
        return false;
    }

    /** Copy distinct active keys into the outtut buffer and stop when it is full. */
    [[nodiscard]] std::size_t copy_unique(std::span<p_key_type> output) const noexcept {
        std::size_t copied = 0;
        for (const observation& candidate : observations_) {
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
        for (const frame& transaction_frame : frames_) {
            count += transaction_frame.active ? 1U : 0U;
        }
        return count;
    }

    void reset() noexcept {
        *this = {};
    }

  private:
    struct frame {
        subtransaction_id id = 0;
        bool active = false;
        bool dirty = false;
    };

    struct observation {
        p_key_type key{};
        subtransaction_id owner = 0;
        bool active = false;
    };

    [[nodiscard]] frame* find_frame(subtransaction_id id) noexcept {
        for (frame& transaction_frame : frames_) {
            if (transaction_frame.active && transaction_frame.id == id) {
                return &transaction_frame;
            }
        }
        return nullptr;
    }

    [[nodiscard]] frame* find_or_create_frame(subtransaction_id id) noexcept {
        if (frame* transaction_frame = find_frame(id); transaction_frame != nullptr) {
            return transaction_frame;
        }
        for (frame& transaction_frame : frames_) {
            if (!transaction_frame.active) {
                transaction_frame = frame{.id = id, .active = true, .dirty = false};
                return &transaction_frame;
            }
        }
        return nullptr;
    }

    void remove_owned_observations(subtransaction_id owner) noexcept {
        for (observation& entry : observations_) {
            if (entry.active && entry.owner == owner) {
                entry = {};
                --observation_count_;
            }
        }
    }

    void make_coarse() noexcept {
        sticky_coarse_ = true;
    }

    std::array<frame, p_frame_capacity> frames_{};
    std::array<observation, p_key_capacity> observations_{};
    std::size_t observation_count_ = 0;
    bool sticky_coarse_ = false;
};

} // namespace bioseq::cache
