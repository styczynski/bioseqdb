/**
 * @file external/provider/bounded_channel.cppm
 * @brief Cancellation-aware byte- and item-bounded producer channel.
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

/// Bounded producer-to-consumer channel with byte and item limits.
///
/// Bytes and items remain charged until the returned lease is released. Timed
/// pops let the consumer poll for interruption without retaining the mutex.
export namespace bioseq::external::provider {

/// Terminal and open states of a bounded provider channel.
enum class channel_state : std::uint8_t { open, done, error, cancelled };

/// Result category returned by a timed channel pop.
enum class pop_status : std::uint8_t {
    item,         // lease is engaged
    closed_done,  // producer finished cleanly, queue drained
    closed_error, // producer reported a terminal error
    cancelled,    // scan cancelled / rescanned
    timed_out     // no item yet, so the caller should poll interrupts and retry
};

/// Byte- and item-bounded producer-to-consumer channel.
template <class p_value_type> class bounded_channel;

/// Move-only item whose lifetime retains its channel capacity charge.
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
    /// Release any current item and transfer another lease into this one.
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
    /// Return the retained capacity charge to the channel.
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
    /// Return the byte charge retained by this lease.
    [[nodiscard]] std::size_t charged_bytes() const noexcept {
        return bytes_;
    }

    /// Return the retained capacity charge before destruction.
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

/// Timed pop status and an optional charged item lease.
template <class p_value_type> struct pop_result {
    pop_status status = pop_status::timed_out;
    std::optional<channel_item_lease<p_value_type>> item;
};

/// Cancellation-aware channel bounded by both bytes and item count.
template <class p_value_type> class bounded_channel {
  public:
    /// Construct a channel with fixed byte and item capacities.
    bounded_channel(std::size_t maximum_bytes, std::size_t maximum_items)
        : maximum_bytes_(maximum_bytes), maximum_items_(maximum_items == 0 ? 1 : maximum_items) {}

    bounded_channel(const bounded_channel&) = delete;
    bounded_channel& operator=(const bounded_channel&) = delete;

    /// Push an item after both byte and item capacity become available.
    ///
    /// A single oversized item is admitted only when the channel is otherwise
    /// empty, preserving progress for the budgeted staging message.
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

    /// Wait for an item up to a caller-selected interrupt polling interval.
    ///
    /// A returned lease keeps the item charged until it is released.
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

    /// Close after successful production and retain queued items for draining.
    void close_done() {
        close(channel_state::done, std::nullopt);
    }
    /// Close with a terminal provider error.
    void close_error(external_error error) {
        close(channel_state::error, std::move(error));
    }
    /// Cancel waiters and prevent further production.
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
    /// Return bytes retained by queued items and active leases.
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

    /// Queued value and its retained byte charge.
    struct entry {
        p_value_type value;
        std::size_t bytes;
    };

    void return_charge(std::size_t bytes) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (charged_items_ > 0) {
            --charged_items_;
        }
        charged_bytes_ = charged_bytes_ >= bytes ? charged_bytes_ - bytes : 0;
        not_full_.notify_one();
    }

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
