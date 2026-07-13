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

import bioseq.external.provider.errors;  // external_error, make_external_error

// DOC0372 — bounded producer/consumer channel with dual byte + item bounds.
//
// A worker task (the sequence retrieval producer) pushes owning messages; the
// PostgreSQL backend thread pops them. `push` BLOCKS when either the byte or item
// bound is reached, giving the streaming pipeline explicit backpressure so peak
// queued memory never exceeds the admitted budget. `pop_for` is a timed wait so
// the PostgreSQL thread can poll `CHECK_FOR_INTERRUPTS` without ever holding the
// channel mutex across a longjmp (which would strand the producer). Bytes/items
// stay CHARGED until the returned `channel_item_lease` is released, so a child
// scan may retain a lease across successive ExecCustomScan calls; the exact
// capacity is returned to the channel when the lease is destroyed.
export namespace bioseq::external::provider {

enum class channel_state : std::uint8_t { open, done, error, cancelled };

enum class pop_status : std::uint8_t {
    item,          // lease is engaged
    closed_done,   // producer finished cleanly, queue drained
    closed_error,  // producer reported a terminal error
    cancelled,     // scan cancelled / rescanned
    timed_out      // no item yet; caller should poll interrupts and retry
};

template <class T>
class bounded_channel;

// Move-only owning lease. Holds the popped value and its charged byte count; the
// destructor returns that capacity to the channel and wakes a blocked producer.
template <class T>
class channel_item_lease {
public:
    channel_item_lease() = default;
    channel_item_lease(const channel_item_lease&) = delete;
    channel_item_lease& operator=(const channel_item_lease&) = delete;

    channel_item_lease(channel_item_lease&& other) noexcept
        : owner_(other.owner_),
          value_(std::move(other.value_)),
          bytes_(other.bytes_) {
        other.owner_ = nullptr;
        other.bytes_ = 0;
    }
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
    ~channel_item_lease() { release(); }

    [[nodiscard]] T& value() noexcept { return value_; }
    [[nodiscard]] const T& value() const noexcept { return value_; }
    [[nodiscard]] std::size_t charged_bytes() const noexcept { return bytes_; }

    void release() noexcept {
        if (owner_ != nullptr) {
            owner_->return_charge(bytes_);
            owner_ = nullptr;
            bytes_ = 0;
        }
    }

private:
    friend class bounded_channel<T>;
    channel_item_lease(bounded_channel<T>* owner, T value,
                       std::size_t bytes) noexcept
        : owner_(owner), value_(std::move(value)), bytes_(bytes) {}

    bounded_channel<T>* owner_ = nullptr;
    T value_{};
    std::size_t bytes_ = 0;
};

template <class T>
struct pop_result {
    pop_status status = pop_status::timed_out;
    std::optional<channel_item_lease<T>> item;
};

template <class T>
class bounded_channel {
public:
    bounded_channel(std::size_t maximum_bytes, std::size_t maximum_items)
        : maximum_bytes_(maximum_bytes),
          maximum_items_(maximum_items == 0 ? 1 : maximum_items) {}

    bounded_channel(const bounded_channel&) = delete;
    bounded_channel& operator=(const bounded_channel&) = delete;

    // Producer: blocks until the item fits under both bounds, the channel is
    // closed, or `stop` fires. A single item larger than the byte bound is
    // admitted only when the channel is otherwise empty (the "one staging
    // message" the admission peak equation accounts for), so it never deadlocks.
    [[nodiscard]] std::expected<void, external_error> push(
        T value, std::size_t bytes, std::stop_token stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this, bytes, &stop] {
            return state_ != channel_state::open || stop.stop_requested() ||
                   (charged_items_ < maximum_items_ &&
                    (charged_bytes_ + bytes <= maximum_bytes_ ||
                     charged_items_ == 0));
        });
        if (state_ == channel_state::cancelled || stop.stop_requested()) {
            return std::unexpected(make_external_error(
                external_error_kind::cancelled, "external stream cancelled"));
        }
        if (state_ != channel_state::open) {
            return std::unexpected(make_external_error(
                external_error_kind::provider_contract,
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

    // Consumer: timed wait so the PG thread can poll interrupts. The returned
    // lease keeps the item charged until released.
    [[nodiscard]] pop_result<T> pop_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool signalled = not_empty_.wait_for(lock, timeout, [this] {
            return !queue_.empty() || state_ != channel_state::open;
        });
        if (!queue_.empty()) {
            entry item = std::move(queue_.front());
            queue_.pop_front();
            // Charge stays until the lease is released.
            return pop_result<T>{pop_status::item,
                                 channel_item_lease<T>(this, std::move(item.value),
                                                       item.bytes)};
        }
        if (!signalled) {
            return pop_result<T>{pop_status::timed_out, std::nullopt};
        }
        switch (state_) {
            case channel_state::done:
                return pop_result<T>{pop_status::closed_done, std::nullopt};
            case channel_state::error:
                return pop_result<T>{pop_status::closed_error, std::nullopt};
            case channel_state::cancelled:
                return pop_result<T>{pop_status::cancelled, std::nullopt};
            case channel_state::open:
                return pop_result<T>{pop_status::timed_out, std::nullopt};
        }
        return pop_result<T>{pop_status::timed_out, std::nullopt};
    }

    void close_done() { close(channel_state::done, std::nullopt); }
    void close_error(external_error error) {
        close(channel_state::error, std::move(error));
    }
    void cancel() { close(channel_state::cancelled, std::nullopt); }

    [[nodiscard]] channel_state state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    [[nodiscard]] std::optional<external_error> error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }
    [[nodiscard]] std::size_t charged_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return charged_bytes_;
    }
    [[nodiscard]] std::size_t high_water_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_bytes_;
    }

private:
    friend class channel_item_lease<T>;

    struct entry {
        T value;
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

}  // namespace bioseq::external::provider
