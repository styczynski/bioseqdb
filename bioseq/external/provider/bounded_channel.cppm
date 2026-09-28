/**
 * @file external/provider/bounded_channel.cppm
 * @brief Queue with byte and item limits for provider output.
 */
module;

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

export module bioseq.external.provider.bounded_channel;

import bioseq.external.provider.errors; // external_error, make_external_error

/// Pass items from a producer to a consumer within byte and item limits.
///
/// Items still count against capacity while the consumer holds their leases.
/// Timed reads let the consumer check for interrupts between waits.
export namespace bioseq::external::provider {

/// Terminal and open states of a bounded provider channel.
enum class channel_state : std::uint8_t { open, done, error, cancelled };

/// Result category returned by a timed channel pop.
enum class pop_status : std::uint8_t {
    item,         // lease is engaged
    closed_done,  // producer finished cleanly, queue drained
    closed_error, // Producer reported an error and the queue has drained.
    cancelled,    // Channel  was cancelled and the queue has drained.
    timed_out     // no item yet, so the caller should poll interrupts and retry
};

/// Queue with byte and item limits.
template <class p_value_type> class bounded_channel;

/// Owns a popped item and holds its channel capacity until release.
/// The channel must outlive the lease.
template <class p_value_type> class channel_item_lease {
  public:
    channel_item_lease() = default;
    channel_item_lease(const channel_item_lease&) = delete;
    channel_item_lease& operator=(const channel_item_lease&) = delete;

    /// Transfer item ownership and its outstanding capacity charge.
    channel_item_lease(channel_item_lease&& other) noexcept
        : owner_(other.owner_), value_(std::move(other.value_)), bytes_(other.bytes_) {
        other.owner_ = nullptr;
        other.bytes_ = 0;
    }
    /// Return the current charge and take ownership of another lease.
    channel_item_lease& operator=(channel_item_lease&& other) noexcept {
        if (this != &other) {
            release();
            owner_ = other.owner_;
            value_ = std::move(other.value_);
            bytes_ = other.bytes_;
            other.owner_ = nullptr;
            other.bytes_ = 0;
        }
        return *this;
    }
    /// Return the outstanding capacity charge to the channel.
    ~channel_item_lease() {
        release();
    }

    /// Return mutable access to the leased item.
    [[nodiscard]] p_value_type& value() noexcept {
        return value_;
    }
    /// Return read-only access to the leased item.
    [[nodiscard]] const p_value_type& value() const noexcept {
        return value_;
    }
    /// Return the number of bytes charged to this lease.
    [[nodiscard]] std::size_t charged_bytes() const noexcept {
        return bytes_;
    }

    /// Return capacity early. The item remains stored in the leasw.
    void release() noexcept {
        if (owner_ != nullptr) {
            owner_->return_charge(bytes_);
            owner_ = nullptr;
            bytes_ = 0;
        }
    }

  private:
    friend class bounded_channel<p_value_type>;
    channel_item_lease(bounded_channel<p_value_type>* owner,
                       p_value_type value,
                       std::size_t bytes) noexcept
        : owner_(owner), value_(std::move(value)), bytes_(bytes) {}

    bounded_channel<p_value_type>* owner_ = nullptr;
    p_value_type value_{};
    std::size_t bytes_ = 0;
};

/// Read status  with a lease when an item was received
template <class p_value_type> struct pop_result {
    pop_status status = pop_status::timed_out;
    std::optional<channel_item_lease<p_value_type>> item;
};

/// Queue whose limits include items still held by consumers.
template <class p_value_type> class bounded_channel {
  public:
    /// Set byte and item capacities, treating a zero item limit as one.
    bounded_channel(std::size_t maximum_bytes, std::size_t maximum_items)
        : maximum_bytes_(maximum_bytes), maximum_items_(maximum_items == 0 ? 1 : maximum_items) {}

    bounded_channel(const bounded_channel&) = delete;
    bounded_channel& operator=(const bounded_channel&) = delete;

    /// Wait for capacity, then queue the item.
    ///
    /// One oversized item can enter when no queued or leased items remain.
    /// This lets a producer pass a staging message larger than the byte limit.
    [[nodiscard]] std::expected<void, external_error>
    push(p_value_type value, std::size_t bytes, std::stop_token stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this, bytes, &stop] {
            return state_ != channel_state::open || stop.stop_requested() ||
                   (charged_items_ < maximum_items_ &&
                    (charged_bytes_ + bytes <= maximum_bytes_ || charged_items_ == 0));
        });
        if (state_ == channel_state::cancelled || stop.stop_requested()) {
            return std::unexpected(
                make_external_error(external_error_kind::cancelled, "external stream cancelled"));
        }
        if (state_ != channel_state::open) {
            return std::unexpected(make_external_error(external_error_kind::provider_contract,
                                                       "push after channel close"));
        }
        queue_.push_back(entry{std::move(value), bytes});
        ++charged_items_;
        charged_bytes_ += bytes;
        if (charged_bytes_ > high_water_bytes_) {
            high_water_bytes_ = charged_bytes_;
        }
        not_empty_.notify_one();
        return {};
    }

    /// Wait for an item up to the interrupt polling interval.
    ///
    /// Queued items remain available after closure. Each returned lease holds
    /// its capacity  charge until release.
    [[nodiscard]] pop_result<p_value_type> pop_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool signalled = not_empty_.wait_for(
            lock, timeout, [this] { return !queue_.empty() || state_ != channel_state::open; });
        if (!queue_.empty()) {
            entry item = std::move(queue_.front());
            queue_.pop_front();
            // Charge stays until the lease is released.
            return pop_result<p_value_type>{
                pop_status::item,
                channel_item_lease<p_value_type>(this, std::move(item.value), item.bytes)};
        }
        if (!signalled) {
            return pop_result<p_value_type>{pop_status::timed_out, std::nullopt};
        }
        switch (state_) {
        case channel_state::done:
            return pop_result<p_value_type>{pop_status::closed_done, std::nullopt};
        case channel_state::error:
            return pop_result<p_value_type>{pop_status::closed_error, std::nullopt};
        case channel_state::cancelled:
            return pop_result<p_value_type>{pop_status::cancelled, std::nullopt};
        case channel_state::open:
            return pop_result<p_value_type>{pop_status::timed_out, std::nullopt};
        }
        return pop_result<p_value_type>{pop_status::timed_out, std::nullopt};
    }

    /// Close after successful production and keep queued items for draining.
    void close_done() {
        close(channel_state::done, std::nullopt);
    }
    /// Close with an error, while keeping queued items available.
    void close_error(external_error error) {
        close(channel_state::error, std::move(error));
    }
    /// Close an open channel as cancelled and wake waiters.
    void cancel() {
        close(channel_state::cancelled, std::nullopt);
    }

    /// Return the current channel state.
    [[nodiscard]] channel_state state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    /// Return the terminal provider error when one was supplied.
    [[nodiscard]] std::optional<external_error> error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }
    /// Return the bytes charged to queued items and active leases.
    [[nodiscard]] std::size_t charged_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return charged_bytes_;
    }
    /// Return the largest simultaneous byte charge observed.
    [[nodiscard]] std::size_t high_water_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_bytes_;
    }

  private:
    friend class channel_item_lease<p_value_type>;

    /// Queued value and its byte charge.
    struct entry {
        p_value_type value;
        std::size_t bytes;
    };

    /// Return capacity from a released lease and wake a producer.
    void return_charge(std::size_t bytes) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (charged_items_ > 0) {
            --charged_items_;
        }
        charged_bytes_ = charged_bytes_ >= bytes ? charged_bytes_ - bytes : 0;
        not_full_.notify_one();
    }

    /// Set the first terrinal state and wake every waiter.
    void close(channel_state new_state, std::optional<external_error> error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == channel_state::open) {
            state_ = new_state;
            error_ = std::move(error);
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<entry> queue_;
    std::size_t charged_bytes_ = 0;
    std::size_t charged_items_ = 0;
    std::size_t high_water_bytes_ = 0;
    const std::size_t maximum_bytes_;
    const std::size_t maximum_items_;
    channel_state state_ = channel_state::open;
    std::optional<external_error> error_;
};

} // namespace bioseq::external::provider
