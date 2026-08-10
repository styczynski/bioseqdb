/**
 * @file
 * @brief Representation-erased lazy sequence views and bounded materialization.
 *
 * Resident owners remain pointer-sized. Fragmented owners retain a bounded
 * number of scanner fragments and rank windows, so memory use depends on the
 * configured fragment size rather than the logical sequence length. Every
 * coordinate is a logical-rank coordinate; unsupported ranges fail before
 * pointer or allocation arithmetic.
 */
module;

#include <hwy/aligned_allocator.h>
#include <libassert/assert.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

export module bioseq.storage.lazy_sequence;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.storage.scanner;
import bioseq.storage.coordinates;
import bioseq.storage.size;
import bioseq.common.fragment_cache;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.rank_pack;

static_assert(__cplusplus >= 202302L);

export namespace bioseq::storage {

enum class sequence_payload_allocation_kind : std::uint8_t {
    admitted_packed_sequence = 0u,
    flat_sequence,
    backing_copy,
};

struct sequence_payload_allocation_event {
    std::uint64_t allocation_id{};
    std::uint64_t input_symbols{};
    std::uint64_t output_symbols{};
    std::uint64_t payload_bytes{};
    std::uint64_t retained_bytes{};
    sequence_payload_allocation_kind kind{
        sequence_payload_allocation_kind::admitted_packed_sequence};
};

/**
 * Allocation-boundary observation without a PostgreSQL dependency.  The two
 * opaque correlations are copied from the planning/executor owner before a
 * task is submitted; the callback interprets them and is required to remain
 * alive until that task has joined/drained.  A null callback is the ordinary
 * non-traced route and costs only the allocation-path null check.
 */
struct sequence_payload_allocation_observer {
    using publish_fn = void (*)(
        void*, sequence_payload_allocation_event const&,
        std::uint64_t, std::uint64_t) noexcept;

    void* context{};
    publish_fn publish{};
    std::uint64_t correlation_a{};
    std::uint64_t correlation_b{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && publish != nullptr;
    }
};

static_assert(std::is_trivially_copyable_v<
              sequence_payload_allocation_observer>);
static_assert(std::is_standard_layout_v<
              sequence_payload_allocation_observer>);

namespace allocation_observer_detail {

inline std::atomic<std::uint64_t> next_allocation_id{1u};

} // namespace allocation_observer_detail

[[nodiscard]] inline std::uint64_t publish_sequence_allocation(
        sequence_payload_allocation_observer const* observer,
        sequence_payload_allocation_kind kind,
        std::uint64_t input_symbols,
        std::uint64_t output_symbols,
        std::uint64_t payload_bytes,
        std::uint64_t retained_bytes) noexcept {
    if (observer == nullptr || !*observer) {
        return 0u;
    }
    std::uint64_t allocation_id =
        allocation_observer_detail::next_allocation_id.fetch_add(
            1u, std::memory_order_relaxed);
    if (allocation_id == 0u) {
        allocation_id =
            allocation_observer_detail::next_allocation_id.fetch_add(
                1u, std::memory_order_relaxed);
    }
    const sequence_payload_allocation_event event{
        .allocation_id = allocation_id,
        .input_symbols = input_symbols,
        .output_symbols = output_symbols,
        .payload_bytes = payload_bytes,
        .retained_bytes = retained_bytes,
        .kind = kind,
    };
    observer->publish(
        observer->context, event,
        observer->correlation_a, observer->correlation_b);
    return allocation_id;
}

template <sequence_alphabet Alphabet>
struct lazy_sequence_traits {
    using alphabet_type = Alphabet;
    using storage_unit = std::uint8_t;
    using packed_alphabet_range = compact_sequence_range<sequence_custom_alphabet<
        Alphabet,
        std::uint8_t,
        seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>)>>;
    using packed_traits = typename packed_alphabet_range::traits;

    static constexpr std::size_t logical_bits_per_symbol =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<Alphabet>);
    static constexpr std::size_t rank_stream_bytes_per_symbol = 1;
    static constexpr std::size_t alphabet_size = seqan3::alphabet_size<Alphabet>;
    static_assert(logical_bits_per_symbol > 0
                  && logical_bits_per_symbol <= CHAR_BIT);

    [[nodiscard]] static storage_unit letter_complement(storage_unit rank) {
        return static_cast<storage_unit>(packed_traits::letter_complement(rank));
    }
};

/**
 * Return the canonical packed-symbol geometry for one facade alphabet and
 * physical storage-unit width.  Facade consumers use this instead of
 * recovering geometry through a concrete compact range's traits.
 *
 * A zero result is the fail-closed spelling for widths outside the supported
 * 1/2/4/8-byte packed ABI.  Non-power-of-two alphabet bit widths intentionally
 * use floor division, matching compact_sequence_range's packed layout.
 */
[[nodiscard]] constexpr std::size_t packed_letters_per_unit(
        std::uint8_t storage_unit_width,
        std::uint8_t bits_per_symbol) noexcept {
    if (!(storage_unit_width == 1u
          || storage_unit_width == 2u
          || storage_unit_width == 4u
          || storage_unit_width == 8u)
        || bits_per_symbol == 0u
        || bits_per_symbol > CHAR_BIT) {
        return 0u;
    }
    return static_cast<std::size_t>(storage_unit_width) * CHAR_BIT
        / bits_per_symbol;
}

template <sequence_alphabet Alphabet>
class lazy_sequence_view {
public:
    using alphabet_type = Alphabet;
    using traits = lazy_sequence_traits<Alphabet>;
    using slice_tag = lazy_sequence_slice_tag;

    using read_at_fn = std::uint8_t const* (*)(
        void const*, sequence_size_t, std::size_t, std::span<std::uint8_t>);

    enum class packed_symbol_order : std::uint8_t {
        least_significant_symbol_first,
    };

    struct resident_packed_sequence {
        void const* units{};
        std::size_t unit_count{};
        sequence_size_t logical_length{};
        std::uint8_t storage_unit_width{};
        std::uint8_t bits_per_symbol{};
        std::uint16_t leading_symbol_offset{};
        std::uint16_t trailing_padding{};
        packed_symbol_order bit_order{
            packed_symbol_order::least_significant_symbol_first};
    };

    using resident_packed_result = std::optional<resident_packed_sequence>;
    using resident_probe_fn = resident_packed_result (*)(
        void const*, sequence_size_t, sequence_size_t, std::uint8_t) noexcept;
    using copy_packed_sequence_fn = bool (*)(
        void const*, void*, std::size_t, std::uint8_t);

    struct backend_ops {
        sequence_size_t (*bp_size)(void const*) noexcept;
        std::array<read_at_fn, 4> read_at;
        resident_probe_fn try_resident_packed;
        copy_packed_sequence_fn copy_packed_sequence;
        bool (*worker_shareable)(void const*) noexcept;
    };

    explicit lazy_sequence_view(
            void const* state,
            backend_ops const* ops,
            sequence_payload_allocation_observer const* observer = nullptr)
            noexcept
        : state_(state), ops_(ops), payload_observer_(observer) {
        DEBUG_ASSERT(
            ops_ != nullptr && ops_->bp_size != nullptr
                && ops_->worker_shareable != nullptr
                && std::ranges::all_of(
                    ops_->read_at,
                    [](read_at_fn callback) { return callback != nullptr; }),
            "lazy sequence backend table must be complete");
    }

    [[nodiscard]] sequence_size_t bp_size() const noexcept {
        return ops_->bp_size(state_);
    }

    [[nodiscard]] sequence_size_t size() const noexcept { return bp_size(); }

    [[nodiscard]] bool worker_shareable() const noexcept {
        return ops_->worker_shareable(state_);
    }

    [[nodiscard]] lazy_sequence_view with_payload_allocation_observer(
            sequence_payload_allocation_observer const* observer) const
            noexcept {
        auto result = *this;
        result.payload_observer_ = observer;
        return result;
    }

    [[nodiscard]] sequence_payload_allocation_observer const*
    payload_allocation_observer() const noexcept {
        return payload_observer_;
    }

    [[nodiscard]] resident_packed_result try_resident_packed(
            sequence_size_t from,
            sequence_size_t to,
            std::uint8_t requested_width) const noexcept {
        if (from > to || to > bp_size()
            || !(requested_width == 1u || requested_width == 2u
                 || requested_width == 4u || requested_width == 8u)
            || ops_->try_resident_packed == nullptr) {
            return std::nullopt;
        }

        resident_packed_result result = ops_->try_resident_packed(
            state_, from, to, requested_width);
        if (!result.has_value()) {
            return std::nullopt;
        }

        const sequence_size_t length = to - from;
        constexpr std::uint8_t bits = static_cast<std::uint8_t>(
            traits::logical_bits_per_symbol);
        const sequence_size_t letters_per_unit =
            static_cast<sequence_size_t>(
                packed_letters_per_unit(requested_width, bits));
        const resident_packed_sequence& region = *result;

        if (letters_per_unit == 0u || region.logical_length != length
            || region.storage_unit_width != requested_width
            || region.bits_per_symbol != bits
            || region.bit_order
                != packed_symbol_order::least_significant_symbol_first) {
            return std::nullopt;
        }
        if (length == 0u) {
            if (region.units != nullptr || region.unit_count != 0u
                || region.leading_symbol_offset != 0u
                || region.trailing_padding != 0u) {
                return std::nullopt;
            }
            return result;
        }

        const auto occupied = bioseq::common::checked_add(
            length,
            static_cast<sequence_size_t>(region.leading_symbol_offset));
        if (region.units == nullptr
            || reinterpret_cast<std::uintptr_t>(region.units)
                    % requested_width
                != 0u
            || region.unit_count
                    > (std::numeric_limits<std::size_t>::max)()
                        / requested_width
            || region.leading_symbol_offset >= letters_per_unit
            || !occupied.has_value()) {
            return std::nullopt;
        }

        const sequence_size_t expected_units = bioseq::common::ceil_div(
            *occupied, letters_per_unit);
        const auto represented_symbols = bioseq::common::checked_mul(
            expected_units, letters_per_unit);
        if (expected_units
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || region.unit_count != static_cast<std::size_t>(expected_units)
            || !represented_symbols.has_value()) {
            return std::nullopt;
        }
        const sequence_size_t trailing =
            *represented_symbols - *occupied;
        if (region.trailing_padding != trailing) {
            return std::nullopt;
        }
        return result;
    }

    /**
     * Ask a backend with a native sequential packed-copy path to populate an
     * exact, aligned full-sequence destination. Unsupported representations
     * return false and leave generic rank streaming as the caller's fallback.
     */
    [[nodiscard]] bool try_copy_packed_sequence(
            std::span<std::uint8_t> destination,
            std::size_t unit_count,
            std::uint8_t requested_width) const {
        if (ops_->copy_packed_sequence == nullptr
            || !(requested_width == 1u || requested_width == 2u
                 || requested_width == 4u || requested_width == 8u)
            || unit_count
                    > (std::numeric_limits<std::size_t>::max)()
                        / requested_width
            || destination.size() != unit_count * requested_width
            || (!destination.empty()
                && reinterpret_cast<std::uintptr_t>(destination.data())
                        % requested_width
                    != 0u)) {
            return false;
        }
        return ops_->copy_packed_sequence(
            state_, destination.data(), unit_count, requested_width);
    }

    template <bool IsReversed, bool WriteReversed = false>
    void copy_ranks(std::span<std::uint8_t> output,
                         std::size_t from,
                         std::size_t to) const {
        if (from > to || to > bp_size() || output.size() < to - from) {
            abort_lazy_sequence_access("lazy sequence rank range is invalid");
        }
        if (from == to) {
            return;
        }
        auto cursor = rank_reader<IsReversed, WriteReversed, std::uint8_t, 2>();
        const std::size_t length = to - from;
        const std::uint8_t* source = cursor.read_at(
            static_cast<sequence_size_t>(from), length, output);
        if (source != output.data()) {
            std::memcpy(output.data(), source, length);
        }
    }

    template <bool IsReversed,
              bool WriteReversed,
              typename StoredType,
              std::size_t MaxLoadedSlots = 2>
    struct rank_window_reader {
        lazy_sequence_view const* parent{};

        [[nodiscard]] StoredType const* read_at(
                sequence_size_t bp_from,
                std::size_t required_length,
                std::span<StoredType> scratch) const {
            static_assert(std::same_as<StoredType, std::uint8_t>);
            static_assert(MaxLoadedSlots == 2,
                          "facade backends implement the two-slot API");
            if (bp_from > parent->bp_size()
                || required_length > parent->bp_size() - bp_from
                || scratch.size() < required_length) {
                abort_lazy_sequence_access("lazy sequence read_at range is invalid");
            }
            constexpr std::size_t orientation =
                (IsReversed ? 2u : 0u) | (WriteReversed ? 1u : 0u);
            const std::uint8_t* result = parent->ops_->read_at[orientation](
                parent->state_, bp_from, required_length, scratch);
            if (required_length != 0u && result == nullptr) {
                abort_lazy_sequence_access("lazy sequence backend returned null");
            }
            return result;
        }
    };

    template <bool IsReversed,
              bool WriteReversed,
              typename StoredType,
              std::size_t MaxLoadedSlots = 2>
    [[nodiscard]] auto rank_reader() const noexcept {
        return rank_window_reader<IsReversed, WriteReversed,
                                StoredType, MaxLoadedSlots>{this};
    }

    class rank_iterator {
        static constexpr std::size_t chunk_size = 256;

        lazy_sequence_view const* parent_{};
        sequence_size_t position_{};
        mutable sequence_size_t loaded_from_{};
        mutable std::size_t loaded_count_{};
        mutable std::array<std::uint8_t, chunk_size> scratch_{};
        mutable std::uint8_t const* loaded_{};

        void ensure_loaded() const {
            if (parent_ == nullptr || position_ >= parent_->bp_size()) {
                abort_lazy_sequence_access(
                    "lazy rank iterator dereferenced outside its sequence");
            }
            if (position_ >= loaded_from_
                && position_ < loaded_from_ + loaded_count_) {
                return;
            }
            loaded_from_ = position_;
            loaded_count_ = static_cast<std::size_t>(
                std::min<sequence_size_t>(
                    chunk_size, parent_->bp_size() - position_));
            loaded_ = parent_->ops_->read_at[0](
                parent_->state_, loaded_from_, loaded_count_, scratch_);
            if (loaded_count_ != 0u && loaded_ == nullptr) {
                abort_lazy_sequence_access("lazy rank backend returned null");
            }
        }

    public:
        using value_type = std::uint8_t;
        using difference_type = std::ptrdiff_t;
        using reference = std::uint8_t;
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;

        rank_iterator() = default;
        rank_iterator(lazy_sequence_view const* parent,
                      sequence_size_t position) noexcept
            : parent_(parent), position_(position) {}
        rank_iterator(rank_iterator const& other) noexcept
            : parent_(other.parent_), position_(other.position_) {}
        rank_iterator& operator=(rank_iterator const& other) noexcept {
            if (this != &other) {
                parent_ = other.parent_;
                position_ = other.position_;
                loaded_from_ = 0u;
                loaded_count_ = 0u;
                loaded_ = nullptr;
            }
            return *this;
        }

        [[nodiscard]] std::uint8_t operator*() const {
            ensure_loaded();
            return loaded_[static_cast<std::size_t>(position_ - loaded_from_)];
        }
        rank_iterator& operator++() noexcept {
            if (parent_ == nullptr || position_ >= parent_->bp_size()) {
                abort_lazy_sequence_access(
                    "lazy rank iterator advanced past end");
            }
            ++position_;
            return *this;
        }
        rank_iterator operator++(int) noexcept {
            rank_iterator previous = *this;
            ++*this;
            return previous;
        }
        rank_iterator& operator+=(difference_type delta) {
            if (parent_ == nullptr) {
                abort_lazy_sequence_access(
                    "unbound lazy rank iterator cannot be advanced");
            }
            if (delta >= 0) {
                const auto magnitude = static_cast<sequence_size_t>(delta);
                if (position_ > parent_->bp_size()
                    || magnitude > parent_->bp_size() - position_) {
                    abort_lazy_sequence_access(
                        "lazy rank iterator advanced past end");
                }
                position_ += magnitude;
            } else {
                const sequence_size_t magnitude = sequence_size_t{0}
                    - static_cast<sequence_size_t>(delta);
                if (magnitude > position_) {
                    abort_lazy_sequence_access(
                        "lazy rank iterator advanced before begin");
                }
                position_ -= magnitude;
            }
            loaded_count_ = 0u;
            loaded_ = nullptr;
            return *this;
        }
        [[nodiscard]] friend bool operator==(
                rank_iterator const& left,
                rank_iterator const& right) noexcept {
            return left.parent_ == right.parent_
                && left.position_ == right.position_;
        }
    };

    [[nodiscard]] rank_iterator begin() const noexcept { return {this, 0u}; }
    [[nodiscard]] rank_iterator end() const noexcept { return {this, bp_size()}; }

    template <iterator_mode Mode>
    [[nodiscard]] rank_iterator iter(rank_iterator value) const noexcept {
        static_assert(Mode == iterator_mode::logical_rank,
                      "facade iter supports logical iterator_mode::logical_rank coordinates only");
        return value;
    }

    template <iterator_mode Mode>
    [[nodiscard]] auto iterable() const noexcept {
        static_assert(Mode == iterator_mode::logical_rank,
                      "facade iteration is a logical-rank iterator_mode::logical_rank range");
        return std::ranges::subrange(begin(), end());
    }

private:
    void const* state_{};
    backend_ops const* ops_{};
    sequence_payload_allocation_observer const* payload_observer_{};
};

template <sequence_alphabet Alphabet>
[[nodiscard]] inline bool is_worker_shareable(
        lazy_sequence_view<Alphabet> value) noexcept {
    return value.worker_shareable();
}

static_assert(lazy_sequence_slice<
              lazy_sequence_view<seq::dna4>>);
static_assert(resident_packed_sequence_view<
              lazy_sequence_view<seq::dna4>>);

template <sequence_alphabet Alphabet>
class retained_lazy_sequence_view {
public:
    using release_fn = void (*)(void*) noexcept;

    retained_lazy_sequence_view(lazy_sequence_view<Alphabet> view,
                             void* lifetime_state,
                             release_fn release) noexcept
        : view_(view), lifetime_state_(lifetime_state), release_(release) {
        DEBUG_ASSERT(lifetime_state_ != nullptr && release_ != nullptr,
                     "lazy view owner requires an adopted backing retain");
    }

    retained_lazy_sequence_view(retained_lazy_sequence_view const&) = delete;
    retained_lazy_sequence_view& operator=(retained_lazy_sequence_view const&) = delete;
    retained_lazy_sequence_view& operator=(retained_lazy_sequence_view&&) = delete;

    retained_lazy_sequence_view(retained_lazy_sequence_view&& other) noexcept
        : view_(other.view_),
          lifetime_state_(other.lifetime_state_),
          release_(other.release_) {
        other.lifetime_state_ = nullptr;
        other.release_ = nullptr;
    }

    ~retained_lazy_sequence_view() noexcept { release_noexcept(); }

    [[nodiscard]] lazy_sequence_view<Alphabet> view() const noexcept {
        return view_;
    }

    void release_noexcept() noexcept {
        if (release_ == nullptr) {
            return;
        }
        void* state = lifetime_state_;
        release_fn release = release_;
        lifetime_state_ = nullptr;
        release_ = nullptr;
        release(state);
    }

private:
    lazy_sequence_view<Alphabet> view_;
    void* lifetime_state_{};
    release_fn release_{};
};

template <class R, class Scanner>
struct lazy_sequence;

/** Pointer-sized owner adapter for an already-resident compact range. */

template <class R, class Scanner>
    requires (!Scanner::is_fragmented())
struct lazy_sequence<R, Scanner> {
    using range_type   = R;
    using scanner_type = Scanner;
    using traits       = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;

    static constexpr std::size_t fragment_cache_capacity = 1u;
    static constexpr std::size_t retained_fragment_limit = 1u;

    const compact_sequence_range<R>* range_ = nullptr;

    [[nodiscard]] [[gnu::always_inline]]
    explicit constexpr lazy_sequence(const compact_sequence_range<R>& ref) noexcept
        : range_(&ref) {}

    /// The underlying range. Algorithm code that has an all-in-RAM fast path
    /// (e.g. the SIMD minimizer kernel) calls this under
    /// `if constexpr (!Scanner::is_fragmented())`.
    [[nodiscard]] [[gnu::always_inline]]
    const compact_sequence_range<R>& underlying() const noexcept { return *range_; }

    // ---- size / shape forwarders --------------------------------------
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t bp_size()   const noexcept { return static_cast<sequence_size_t>(range_->bp_size()); }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t size()      const noexcept { return static_cast<sequence_size_t>(range_->size()); }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t byte_size() const noexcept { return static_cast<sequence_size_t>(range_->byte_size()); }
    [[nodiscard]] [[gnu::always_inline]]
    std::uint16_t   padding()   const noexcept { return static_cast<std::uint16_t>(range_->padding()); }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t fragment_count() const noexcept { return 1; }

    // ---- iterator (the underlying range's iterator) -------------------
    using iterator_impl = typename compact_sequence_range<R>::iterator;
    [[nodiscard]] [[gnu::always_inline]] iterator_impl cbegin() const noexcept { return range_->cbegin(); }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl cend()   const noexcept { return range_->cend();   }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl begin()  const noexcept { return range_->cbegin(); }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl end()    const noexcept { return range_->cend();   }

    template <iterator_mode M, class I>
    [[nodiscard]] [[gnu::always_inline]] auto iter(const I& it) const noexcept
        { return range_->template iter<M>(it); }
    template <iterator_mode M>
    [[nodiscard]] [[gnu::always_inline]] auto iterable() const noexcept
        { return range_->template iterable<M>(); }

    template <class I>
    [[nodiscard]] [[gnu::always_inline]] std::string to_string(I b, I e, bool split_units = false) const
        { return range_->to_string(b, e, split_units); }

    // ---- bit-cast view (exposed on the no-fragment side only) ---------
    template <class U>
    [[nodiscard]] [[gnu::always_inline]] auto bit_cast_to_storage_unit() const noexcept
        { return range_->template bit_cast_to_storage_unit<U>(); }

    // ---- encode forwarders (the aligner-facing API) -------------------
    template <bool is_reversed, bool write_reversed = false>
    [[gnu::always_inline]]
    void copy_ranks(
        std::span<std::uint8_t> destination,
        std::size_t from,
        std::size_t to) const
    {
        if (from > to || to > bp_size() || destination.size() < to - from) {
            abort_lazy_sequence_access("resident lazy rank range is invalid");
        }
        if (from == to) {
            return;
        }
        range_->template to_encoded_into<is_reversed, write_reversed>(
            destination, from, to);
    }

    /** Rank-window reader that writes resident ranks into caller scratch. */
    template <bool is_reversed, bool write_reversed, class stored_type_t, std::size_t MaxLoadedSlots = 2>
    struct rank_window_reader {
        const lazy_sequence* parent_ = nullptr;
        explicit rank_window_reader(const lazy_sequence* parent) noexcept : parent_(parent) {}
        [[nodiscard]] const stored_type_t* read_at(sequence_size_t bp_from,
                                                  std::size_t required_length,
                                                  std::span<stored_type_t> scratch) const {
            if (bp_from > parent_->bp_size()
                || required_length > parent_->bp_size() - bp_from
                || scratch.size() < required_length
                || bp_from > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
                abort_lazy_sequence_access("resident lazy rank range is invalid");
            }
            if (required_length == 0u) {
                return scratch.data();
            }
            static_assert(sizeof(stored_type_t) == 1,
                "no-fragment rank_reader::read_at expects a 1-byte encoded type");
            parent_->range_->template to_encoded_into<is_reversed, write_reversed>(
                std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(scratch.data()), required_length),
                static_cast<std::size_t>(bp_from),
                static_cast<std::size_t>(bp_from) + required_length);
            return scratch.data();
        }
    };
    template <bool is_reversed, bool write_reversed, class stored_type_t, std::size_t MaxLoadedSlots = 2>
    [[nodiscard]] auto rank_reader() const noexcept {
        return rank_window_reader<is_reversed, write_reversed, stored_type_t, MaxLoadedSlots>(this);
    }

    // ---- read_cptr — both overloads forward to the resident bytes -----
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it) const noexcept {
        return compact_sequence_range<R>::read_cptr(it);
    }
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it,
                                  std::span<storage_unit> /*scratch unused*/,
                                  std::size_t /*required_length unused*/) const noexcept {
        return compact_sequence_range<R>::read_cptr(it);
    }
};
/** Bounded-cache owner adapter for a fragmented scanner. */

template <class R, class Scanner>
    requires (Scanner::is_fragmented())
struct lazy_sequence<R, Scanner> {
    using range_type   = R;
    using scanner_type = Scanner;
    using traits       = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;
    using fragment_type     = sequence_fragment<R>;

    static constexpr std::size_t fragment_storage_units =
        Scanner::fragment_storage_units;
    static constexpr std::size_t fragment_cache_capacity = 16u;
    static constexpr std::size_t retained_fragment_limit = 1u;
    static_assert(fragment_cache_capacity > retained_fragment_limit,
                  "buffer capacity must exceed the post-eviction target count");

    static constexpr std::size_t letters_per_unit = traits::letters_per_unit;
    static constexpr std::size_t bits_per_symbol = traits::bits_per_char;
    static_assert(
        fragment_storage_units
        <= (std::numeric_limits<std::size_t>::max)() / letters_per_unit,
        "fragment rank capacity must fit size_t");
    static constexpr std::size_t fragment_logical_capacity =
        fragment_storage_units * letters_per_unit;

    // Iterators and rank readers may retain cache-slot references, so the
    // scanner owner has a stable address for its complete lifetime.
    lazy_sequence(const lazy_sequence&)            = delete;
    lazy_sequence& operator=(const lazy_sequence&) = delete;
    lazy_sequence(lazy_sequence&&)                 = delete;
    lazy_sequence& operator=(lazy_sequence&&)      = delete;

    explicit lazy_sequence(Scanner&& scanner) noexcept
        : scanner_(std::move(scanner)) {}
    ~lazy_sequence() = default;

    // ---- size / shape -------------------------------------------------
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t bp_size()   const noexcept { return scanner_.bp_size(); }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t size()      const noexcept { return scanner_.storage_unit_count(); }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t byte_size() const noexcept { return scanner_.storage_byte_count(); }
    [[nodiscard]] [[gnu::always_inline]]
    std::uint16_t   padding()   const noexcept {
        return static_cast<std::uint16_t>(scanner_.trailing_padding());
    }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t fragment_count() const noexcept { return scanner_.fragment_count(); }
    [[nodiscard]] const Scanner& scanner() const noexcept { return scanner_; }

    /** Packed-unit iterator carrying an owner-qualified global unit index. */
    struct iterator_impl {
        const lazy_sequence* parent_      = nullptr;
        sequence_size_t              global_unit_ = 0;

        [[gnu::always_inline]] iterator_impl& operator++() noexcept {
            return *this += 1u;
        }
        [[gnu::always_inline]] iterator_impl& operator+=(
            sequence_size_t count) noexcept
        {
            if (parent_ == nullptr || global_unit_ > parent_->size()
                || count > parent_->size() - global_unit_) {
                abort_lazy_sequence_access(
                    "packed sequence iterator advanced past end");
            }
            global_unit_ += count;
            return *this;
        }

        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t current_fragment_index() const noexcept {
            return global_unit_ / fragment_storage_units;
        }
        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t current_fragment_unit_offset() const noexcept {
            return global_unit_ % fragment_storage_units;
        }
        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t global_unit_index() const noexcept { return global_unit_; }

        [[nodiscard]] [[gnu::always_inline]]
        friend bool operator==(const iterator_impl& a, const iterator_impl& b) noexcept {
            return a.parent_ == b.parent_ && a.global_unit_ == b.global_unit_;
        }
        [[nodiscard]] [[gnu::always_inline]]
        friend std::strong_ordering operator<=>(const iterator_impl& a, const iterator_impl& b) noexcept {
            if (a.parent_ != b.parent_) {
                abort_lazy_sequence_access("lazy iterator: cross-parent comparison");
            }
            return a.global_unit_ <=> b.global_unit_;
        }
    };

    [[nodiscard]] iterator_impl cbegin() const noexcept { return iterator_impl{this, 0}; }
    [[nodiscard]] iterator_impl cend()   const noexcept { return iterator_impl{this, size()}; }
    [[nodiscard]] iterator_impl begin()  const noexcept { return cbegin(); }
    [[nodiscard]] iterator_impl end()    const noexcept { return cend(); }

    // ---- fragment management ------------------------------------------
    [[nodiscard]] const fragment_type& ensure_fragment(std::size_t n) const {
        if (n >= scanner_.fragment_count()) {
            abort_lazy_sequence_access("fragment index is out of range");
        }
        auto key_of = [](const fragment_type& h) noexcept { return h.fragment_index; };
        if (auto found = fragments_.find(n, key_of); found.has_value()) {
            return fragments_.slot_at(found->first);
        }
        load_fragment(n);
        auto found2 = fragments_.find(n, key_of);
        if (!found2.has_value()) {
            abort_lazy_sequence_access("lazy: ensure_fragment failed to load fragment");
        }
        return fragments_.slot_at(found2->first);
    }

    void evict_old_fragments() const {
        while (fragments_.valid_count() > retained_fragment_limit) {
            fragments_.evict_oldest([this](fragment_type& evicted) noexcept {
                scanner_.release_fragment(evicted.fragment_index);
            });
        }
    }

    /** Read contiguous packed units, stitching across fragments when needed. */
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it,
                                  std::span<storage_unit> scratch,
                                  std::size_t required_length) const {
        if (it.parent_ != this) {
            abort_lazy_sequence_access("read_cptr: iterator's parent doesn't match this wrapper");
        }
        if (it.global_unit_ > size()
            || required_length > size() - it.global_unit_
            || scratch.size() < required_length) {
            abort_lazy_sequence_access("packed sequence read range is invalid");
        }
        if (required_length == 0u) {
            return scratch.data();
        }
        const sequence_size_t fragment_index =
            it.global_unit_ / fragment_storage_units;
        if (fragment_index > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            abort_lazy_sequence_access(
                "packed sequence fragment index is unsupported");
        }
        const std::size_t cur_frag = static_cast<std::size_t>(fragment_index);
        const std::size_t in_frag_offset = static_cast<std::size_t>(it.global_unit_ % fragment_storage_units);

        const fragment_type& h = ensure_fragment(cur_frag);
        // FAST PATH — request fits inside the current fragment. One compare.
        if (in_frag_offset <= h.data.size()
            && required_length <= h.data.size() - in_frag_offset) {
            return h.data.data() + in_frag_offset;
        }
        // SLOW PATH — stitch into the caller's scratch span.
        std::size_t copied        = 0;
        std::size_t cur_unit      = in_frag_offset;
        std::size_t cur_frag_no   = cur_frag;
        while (copied < required_length) {
            const fragment_type& cur_h  = ensure_fragment(cur_frag_no);
            const std::size_t avail   = cur_h.data.size() - cur_unit;
            const std::size_t to_copy = std::min(avail, required_length - copied);
            std::memcpy(scratch.data() + copied, cur_h.data.data() + cur_unit,
                        to_copy * sizeof(storage_unit));
            copied += to_copy;
            cur_frag_no += 1;
            cur_unit = 0;
            if (cur_frag_no >= scanner_.fragment_count() && copied < required_length) {
                abort_lazy_sequence_access("read_cptr: required_length extends past end of sequence");
            }
        }
        evict_old_fragments();
        return scratch.data();
    }

    // Single-arg read_cptr — points into the current fragment (no stitch).
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it) const {
        if (it.parent_ != this || it.global_unit_ >= size()) {
            abort_lazy_sequence_access("packed sequence iterator is out of range");
        }
        const sequence_size_t fragment_index =
            it.global_unit_ / fragment_storage_units;
        if (fragment_index > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            abort_lazy_sequence_access(
                "packed sequence fragment index is unsupported");
        }
        const std::size_t cur_frag = static_cast<std::size_t>(fragment_index);
        const std::size_t in_frag_offset = static_cast<std::size_t>(it.global_unit_ % fragment_storage_units);
        const fragment_type& h = ensure_fragment(cur_frag);
        return h.data.data() + in_frag_offset;
    }

    // ---- forward iterator_mode::logical_rank iterator (drives the scalar minimizer kernel) -----
    // Streams letters across fragments in global order; iterates exactly
    // bp_size() letters (matching compact_sequence_range's iter<iterator_mode::logical_rank> end, which
    // excludes trailing padding — sequences.cppm:1034). letter 0 sits at the
    // LSB of each storage unit (LSB-first, matching bp_scalar_iter).
    struct logical_rank_iterator_impl {
        const lazy_sequence* parent_   = nullptr;
        sequence_size_t              global_rank_ = 0;

        [[nodiscard]] storage_unit operator*() const {
            if (parent_ == nullptr || global_rank_ >= parent_->bp_size()) {
                abort_lazy_sequence_access(
                    "logical-rank iterator dereferenced at end");
            }
            const sequence_size_t fragment_index =
                global_rank_ / fragment_logical_capacity;
            if (fragment_index
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
                abort_lazy_sequence_access(
                    "logical-rank fragment index is unsupported");
            }
            const std::size_t frag =
                static_cast<std::size_t>(fragment_index);
            const fragment_type& h = parent_->ensure_fragment(frag);
            const std::size_t bp_in_frag    = static_cast<std::size_t>(global_rank_ % fragment_logical_capacity);
            const std::size_t unit_in_frag = bp_in_frag / letters_per_unit;
            const std::size_t letter_in_unit = bp_in_frag % letters_per_unit;
            const storage_unit u = h.data[unit_in_frag];
            return static_cast<storage_unit>(
                (u >> (letter_in_unit * bits_per_symbol)) & traits::char_mask);
        }
        [[gnu::always_inline]] logical_rank_iterator_impl& operator++() noexcept {
            if (parent_ == nullptr || global_rank_ >= parent_->bp_size()) {
                abort_lazy_sequence_access(
                    "logical-rank iterator advanced past end");
            }
            ++global_rank_;
            return *this;
        }
        [[nodiscard]] [[gnu::always_inline]]
        friend bool operator==(const logical_rank_iterator_impl& a, const logical_rank_iterator_impl& b) noexcept {
            return a.parent_ == b.parent_ && a.global_rank_ == b.global_rank_;
        }
    };

    [[nodiscard]] logical_rank_iterator_impl rank_begin() const noexcept { return logical_rank_iterator_impl{this, 0}; }
    [[nodiscard]] logical_rank_iterator_impl rank_end()   const noexcept { return logical_rank_iterator_impl{this, bp_size()}; }

    // iter<iterator_mode::logical_rank> / iterable<iterator_mode::logical_rank> — mirror compact_sequence_range_base's surface so
    // the scalar minimizer kernel can run over the fragmented wrapper unchanged.
    template <iterator_mode M, class I>
    [[nodiscard]] logical_rank_iterator_impl iter(const I& it) const noexcept {
        static_assert(M == iterator_mode::logical_rank,
                      "fragmented lazy iter<M>: only iterator_mode::logical_rank iteration is supported "
                      "(the minimizer kernel consumes ranks). iterator_mode::alphabet_symbol is not "
                      "exercised on the fragmented path.");
        if (it.global_unit_ >= size()) {
            return logical_rank_iterator_impl{this, bp_size()};
        }
        const auto logical_rank = bioseq::common::checked_mul(
            it.global_unit_,
            static_cast<sequence_size_t>(letters_per_unit));
        if (!logical_rank.has_value() || *logical_rank > bp_size()) {
            abort_lazy_sequence_access(
                "packed iterator cannot be mapped to a logical rank");
        }
        return logical_rank_iterator_impl{this, *logical_rank};
    }

    struct logical_rank_range_view {
        logical_rank_iterator_impl begin_;
        logical_rank_iterator_impl end_;
        [[nodiscard]] logical_rank_iterator_impl begin() const noexcept {
            return begin_;
        }
        [[nodiscard]] logical_rank_iterator_impl end() const noexcept {
            return end_;
        }
    };
    template <iterator_mode M>
    [[nodiscard]] logical_rank_range_view iterable() const noexcept {
        static_assert(M == iterator_mode::logical_rank, "fragmented lazy iterable<M>: only iterator_mode::logical_rank supported");
        return logical_rank_range_view{rank_begin(), rank_end()};
    }

    /** Copy a logical rank interval without materializing the full sequence. */
    template <bool is_reversed, bool write_reversed = false>
    void copy_ranks(
        std::span<std::uint8_t> destination,
        std::size_t from,
        std::size_t to) const
    {
        if (from > to || to > bp_size() || destination.size() < to - from) {
            abort_lazy_sequence_access("fragmented lazy rank range is invalid");
        }
        if (from == to) {
            return;
        }
        const std::size_t length = to - from;
        auto reader = rank_reader<
            is_reversed, write_reversed, std::uint8_t, 2>();
        const std::uint8_t* ranks = reader.read_at(
            static_cast<sequence_size_t>(from), length, destination);
        if (ranks != destination.data()) {
            std::memcpy(destination.data(), ranks, length);
        }
    }

    /** Reject lane reinterpretation because lanes may span fragments. */
    template <class U>
    auto bit_cast_to_storage_unit() const noexcept {
        static_assert(sizeof(U) == 0,
            "lazy_sequence::bit_cast_to_storage_unit is unavailable on the "
            "fragmented specialisation (lanes spread across fragments). Gate the "
            "bit-cast path via `if constexpr (Scanner::is_fragmented())` and "
            "take the fragmented path.");
    }

    /**
     * Per-fragment forward-rank cache with a compile-time resident-slot bound.
     * Plain forward reads fitting one slot borrow it directly. Oriented and
     * cross-fragment reads stream into caller scratch and never allocate in
     * proportion to the complete logical sequence.
     */
    template <bool is_reversed, bool write_reversed, class stored_type_t, std::size_t MaxLoadedSlots = 2>
    struct rank_window_reader {
        static_assert(MaxLoadedSlots >= 1u);
        struct encoded_slot {
            stored_type_t* bytes       = nullptr;          // malloc'd: raw forward ranks
            std::size_t    fragment_no = std::numeric_limits<std::size_t>::max();
            std::size_t    bp_count    = 0;
        };

        const lazy_sequence* parent_ = nullptr;
        mutable bioseq::common::fragment_cache<encoded_slot, MaxLoadedSlots + 1> slots_{};

        explicit rank_window_reader(const lazy_sequence* parent) noexcept : parent_(parent) {}
        rank_window_reader(const rank_window_reader&)            = delete;
        rank_window_reader& operator=(const rank_window_reader&) = delete;
        rank_window_reader(rank_window_reader&&)                 = delete;
        rank_window_reader& operator=(rank_window_reader&&)      = delete;
        ~rank_window_reader() {
            // Free every resident slot's bytes.
            for (std::size_t i = 0; i < slots_.capacity; ++i) {
                slots_.evict_at(i, [](encoded_slot& s) noexcept { std::free(s.bytes); s.bytes = nullptr; });
            }
        }

        // Encode fragment `frag` as RAW FORWARD ranks (no complement, no
        // reorder) so a resident slot is reusable across (is_reversed,
        // write_reversed); the orientation transform is applied by read_at.
        encoded_slot& ensure_rank_slot(std::size_t frag) const {
            auto key_of = [](const encoded_slot& s) noexcept { return s.fragment_no; };
            if (auto found = slots_.find(frag, key_of); found.has_value()) {
                return slots_.slot_at(found->first);
            }
            const fragment_type& h = parent_->ensure_fragment(frag);
            const std::size_t bp_count = static_cast<std::size_t>(h.logical_length);
            const auto allocation_bytes = bioseq::common::checked_mul(
                bp_count, sizeof(stored_type_t));
            if (!allocation_bytes.has_value()) {
                abort_lazy_sequence_access(
                    "rank-window allocation size is unsupported");
            }
            stored_type_t* bytes = static_cast<stored_type_t*>(
                std::malloc(*allocation_bytes));
            if (bytes == nullptr) { abort_lazy_sequence_access("rank_window_reader: slot malloc failed"); }
            for (std::size_t bp = 0; bp < bp_count; ++bp) {
                const std::size_t unit_in_frag = bp / letters_per_unit;
                const std::size_t letter_in_unit = bp % letters_per_unit;
                const storage_unit u = h.data[unit_in_frag];
                const storage_unit rank = static_cast<storage_unit>(
                    (u >> (letter_in_unit * bits_per_symbol))
                    & traits::char_mask);
                bytes[bp] = static_cast<stored_type_t>(rank);
            }
            auto [idx, gen] = slots_.push(encoded_slot{bytes, frag, bp_count},
                [](encoded_slot& evicted) noexcept { std::free(evicted.bytes); evicted.bytes = nullptr; });
            (void)gen;
            // Aggressive eviction: keep at most MaxLoadedSlots resident.
            while (slots_.valid_count() > MaxLoadedSlots) {
                slots_.evict_oldest([](encoded_slot& evicted) noexcept {
                    std::free(evicted.bytes); evicted.bytes = nullptr;
                });
            }
            return slots_.slot_at(idx);
        }

        // read_at(bp_from, len, scratch): `bp_from` is in the SAME coordinate
        // convention as compact_sequence_range::to_encoded_into — region
        // coordinates of the (possibly reversed) OUTPUT sequence. Returns a
        // pointer to `len` encoded bytes: a resident slot (zero-copy) for the
        // plain-forward fast path, else `scratch.data()`.
        [[nodiscard]]
        const stored_type_t* read_at(sequence_size_t bp_from, std::size_t required_length,
                                    std::span<stored_type_t> scratch) const {
            if (bp_from > parent_->bp_size()
                || required_length > parent_->bp_size() - bp_from
                || scratch.size() < required_length) {
                abort_lazy_sequence_access("fragmented lazy rank range is invalid");
            }
            if (required_length == 0u) {
                return scratch.data();
            }
            if (parent_->bp_size()
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
                abort_lazy_sequence_access(
                    "rank-window coordinates exceed size_t");
            }
            const std::size_t total =
                static_cast<std::size_t>(parent_->bp_size());
            // Reverse strand: output region [bp_from, bp_from+len) maps to the
            // FORWARD source coords [total-(bp_from+len), total-bp_from)
            // (sequences.cppm:1341). Forward strand reads in place.
            const std::size_t fwd_from = is_reversed
                ? (total - (static_cast<std::size_t>(bp_from) + required_length))
                : static_cast<std::size_t>(bp_from);
            constexpr bool descending_write =
                (is_reversed && !write_reversed) || (!is_reversed && write_reversed);

            // §5 zero-copy fast path: plain-forward encode fitting one slot.
            if constexpr (!is_reversed && !write_reversed) {
                const std::size_t fragment_bp = static_cast<std::size_t>(parent_->fragment_logical_capacity);
                const std::size_t cur_frag    = fwd_from / fragment_bp;
                const std::size_t in_frag_bp  = fwd_from % fragment_bp;
                const encoded_slot& slot = ensure_rank_slot(cur_frag);
                if (in_frag_bp + required_length <= slot.bp_count) {
                    return slot.bytes + in_frag_bp;
                }
            }
            // Forward-sequential per-fragment gather of raw ranks for
            // [fwd_from, fwd_from+len); apply emit_letter (complement iff
            // is_reversed; descending output iff descending_write).
            const std::size_t fragment_bp = static_cast<std::size_t>(parent_->fragment_logical_capacity);
            std::size_t gathered = 0;
            std::size_t src_bp   = fwd_from;
            while (gathered < required_length) {
                const std::size_t frag    = src_bp / fragment_bp;
                const std::size_t in_frag = src_bp % fragment_bp;
                const encoded_slot& s = ensure_rank_slot(frag);
                const std::size_t avail   = s.bp_count - in_frag;
                const std::size_t to_copy = std::min(avail, required_length - gathered);
                for (std::size_t i = 0; i < to_copy; ++i) {
                    const stored_type_t raw = s.bytes[in_frag + i];
                    const stored_type_t letter = is_reversed
                        ? static_cast<stored_type_t>(
                              traits::letter_complement(static_cast<storage_unit>(raw)))
                        : raw;
                    const std::size_t out_idx = gathered + i;
                    scratch[descending_write ? (required_length - 1 - out_idx) : out_idx] = letter;
                }
                gathered += to_copy;
                src_bp   += to_copy;
                if (gathered < required_length && src_bp >= total) {
                    abort_lazy_sequence_access("read_at: required_length extends past end of sequence");
                }
            }
            return scratch.data();
        }

        [[nodiscard]] stored_type_t operator[](sequence_size_t bp_idx) const {
            stored_type_t one{};
            std::span<stored_type_t> sc(&one, 1);
            const stored_type_t* p = read_at(bp_idx, 1, sc);
            return p[0];
        }
    };

    template <bool is_reversed, bool write_reversed, class stored_type_t, std::size_t MaxLoadedSlots = 2>
    [[nodiscard]] auto rank_reader() const noexcept {
        return rank_window_reader<is_reversed, write_reversed, stored_type_t, MaxLoadedSlots>(this);
    }

private:
    mutable Scanner scanner_;
    mutable bioseq::common::fragment_cache<fragment_type, fragment_cache_capacity> fragments_{};

    void load_fragment(std::size_t n) const {
        const auto logical_offset = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(n),
            static_cast<std::uint64_t>(fragment_logical_capacity));
        if (!logical_offset.has_value()
            || *logical_offset > scanner_.bp_size()) {
            abort_lazy_sequence_access("fragment logical offset is invalid");
        }
        fragment_type fragment;
        fragment.data = scanner_.load_fragment(n);
        fragment.fragment_index = n;
        fragment.logical_offset = *logical_offset;
        const bool is_last = n == scanner_.fragment_count() - 1u;
        fragment.logical_length = is_last
            ? (scanner_.bp_size() - fragment.logical_offset)
            : static_cast<sequence_size_t>(fragment_logical_capacity);
        fragment.trailing_padding = is_last
            ? static_cast<std::uint16_t>(scanner_.trailing_padding())
            : std::uint16_t{0};
        const auto fragment_capacity = bioseq::common::checked_mul(
            static_cast<std::uint64_t>(fragment.data.size()),
            static_cast<std::uint64_t>(letters_per_unit));
        if (!fragment_capacity.has_value()
            || fragment.data.empty()
            || fragment.data.size() > fragment_storage_units
            || (!is_last
                && fragment.data.size() != fragment_storage_units)
            || fragment.trailing_padding >= letters_per_unit
            || fragment.logical_length > *fragment_capacity
            || *fragment_capacity - fragment.logical_length
                != fragment.trailing_padding) {
            abort_lazy_sequence_access("scanner returned invalid fragment geometry");
        }
        fragments_.push(fragment, [this](fragment_type& evicted) noexcept {
            scanner_.release_fragment(evicted.fragment_index);
        });
        evict_old_fragments();
    }
};

/** Factory and callback dispatch helpers for stable-address lazy owners. */

/// Wrap an in-memory range in the zero-overhead no-fragment lazy wrapper.
template <class R>
[[nodiscard]] [[gnu::always_inline]]
inline lazy_sequence<R, resident_sequence_scanner<
        typename compact_sequence_range<R>::traits::storage_unit>>
make_resident_lazy_sequence(const compact_sequence_range<R>& range) noexcept {
    return lazy_sequence<R, resident_sequence_scanner<
        typename compact_sequence_range<R>::traits::storage_unit>>{range};
}

/// Construct an in-memory lazy wrapper around `range` and invoke `fn(lazy)`.
/// Zero-cost: the wrapper is a thin reference.
template <class R, class Fn>
[[gnu::always_inline]]
inline void with_resident_lazy_sequence(const compact_sequence_range<R>& range, Fn&& fn) {
    lazy_sequence<R, resident_sequence_scanner<
        typename compact_sequence_range<R>::traits::storage_unit>> lazy{range};
    std::forward<Fn>(fn)(lazy);
}

template <class R, class Scanner>
[[nodiscard]] inline bool has_resident_sequence_bytes(
        const lazy_sequence<R, Scanner>& sequence) noexcept {
    if constexpr (Scanner::is_fragmented()) {
        if constexpr (requires (const Scanner& scanner) {
                { scanner.is_in_memory_mode() } -> std::convertible_to<bool>;
            }) {
            return sequence.scanner().is_in_memory_mode();
        } else {
            return true;
        }
    } else {
        return true;
    }
}

template <class R, class Scanner>
[[nodiscard]] inline bool is_worker_shareable(
        const lazy_sequence<R, Scanner>&) noexcept {
    return !Scanner::is_fragmented();
}

namespace lazy_sequence_detail {

template <class Alphabet>
concept complementable_sequence_alphabet = requires (Alphabet value) {
    seqan3::complement(std::move(value));
};

static_assert(complementable_sequence_alphabet<seq::dna4>);
static_assert(complementable_sequence_alphabet<seq::dna5>);
static_assert(!complementable_sequence_alphabet<seq::aa27>);

template <bool IsReversed, bool WriteReversed, class R, class Scanner>
[[nodiscard]] const std::uint8_t* owned_sequence_read_at(
        void const* state,
        sequence_size_t bp_from,
        std::size_t required_length,
        std::span<std::uint8_t> scratch) {
    const auto& sequence =
        *static_cast<const lazy_sequence<R, Scanner>*>(state);
    if (bp_from > sequence.bp_size()
        || required_length > sequence.bp_size() - bp_from
        || scratch.size() < required_length) {
        abort_lazy_sequence_access(
            "owned lazy sequence rank range is invalid");
    }
    if (required_length == 0u) {
        return scratch.data();
    }
    using alphabet_type =
        typename compact_sequence_range<R>::traits::alphabet_type;
    if constexpr (IsReversed
                  && !complementable_sequence_alphabet<alphabet_type>) {
        throw std::invalid_argument(
            "reverse-complement encoding requires a complementable alphabet");
    } else {
        auto cursor = sequence.template rank_reader<
            IsReversed, WriteReversed, std::uint8_t, 2>();
        const std::uint8_t* result = cursor.read_at(
            bp_from, required_length, scratch);
        // A fragmented encoded iterator owns its resident slot ring. The named
        // temporary dies at callback return, so only caller scratch may escape.
        if (result != scratch.data()) {
            std::memcpy(scratch.data(), result, required_length);
        }
        return scratch.data();
    }
}

template <class R, class Scanner>
[[nodiscard]] sequence_size_t owned_sequence_bp_size(
        void const* state) noexcept {
    return static_cast<const lazy_sequence<R, Scanner>*>(state)
        ->bp_size();
}

template <class R, class Scanner>
[[nodiscard]] bool owned_sequence_worker_shareable(
        void const* state) noexcept {
    const auto& sequence =
        *static_cast<const lazy_sequence<R, Scanner>*>(state);
    return is_worker_shareable(sequence);
}

template <class R, class Scanner>
[[nodiscard]] auto owned_sequence_resident_probe(
        void const* state,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width) noexcept
    -> typename lazy_sequence_view<
        typename compact_sequence_range<R>::traits::alphabet_type>::
        resident_packed_result {
    using range_traits = typename compact_sequence_range<R>::traits;
    using alphabet_type = typename range_traits::alphabet_type;
    using view_type = lazy_sequence_view<alphabet_type>;
    using descriptor_type = typename view_type::resident_packed_sequence;

    if constexpr (Scanner::is_fragmented()) {
        // Fragment-cache slots can be evicted by later reads and therefore do
        // not meet the facade-lifetime stability contract.
        (void)state;
        (void)from;
        (void)to;
        (void)requested_width;
        return std::nullopt;
    } else {
        constexpr std::uint8_t physical_width =
            static_cast<std::uint8_t>(sizeof(typename range_traits::storage_unit));
        constexpr std::uint8_t logical_bits = static_cast<std::uint8_t>(
            lazy_sequence_traits<alphabet_type>::logical_bits_per_symbol);
        if (requested_width != physical_width
            || range_traits::bits_per_char != logical_bits) {
            return std::nullopt;
        }

        const sequence_size_t length = to - from;
        if (length == 0u) {
            return descriptor_type{
                .units = nullptr,
                .unit_count = 0u,
                .logical_length = 0u,
                .storage_unit_width = physical_width,
                .bits_per_symbol = logical_bits,
                .leading_symbol_offset = 0u,
                .trailing_padding = 0u,
                .bit_order = view_type::packed_symbol_order::
                    least_significant_symbol_first,
            };
        }

        constexpr sequence_size_t letters_per_unit =
            range_traits::letters_per_unit;
        const sequence_size_t first_unit = from / letters_per_unit;
        const sequence_size_t leading = from % letters_per_unit;
        const auto occupied = bioseq::common::checked_add(leading, length);
        if (!occupied.has_value()) {
            return std::nullopt;
        }
        const sequence_size_t unit_count = bioseq::common::ceil_div(
            *occupied, letters_per_unit);
        const auto represented_symbols = bioseq::common::checked_mul(
            unit_count, letters_per_unit);
        if (!represented_symbols.has_value()) {
            return std::nullopt;
        }
        const sequence_size_t trailing =
            *represented_symbols - *occupied;

        const auto& sequence =
            *static_cast<const lazy_sequence<R, Scanner>*>(state);
        const auto& range = sequence.underlying();
        if (first_unit > range.size()
            || unit_count > range.size() - first_unit
            || unit_count
                    > static_cast<sequence_size_t>(
                        (std::numeric_limits<std::size_t>::max)())) {
            return std::nullopt;
        }
        const auto* base = range.read_cptr(range.cbegin());
        return descriptor_type{
            .units = base + static_cast<std::size_t>(first_unit),
            .unit_count = static_cast<std::size_t>(unit_count),
            .logical_length = length,
            .storage_unit_width = physical_width,
            .bits_per_symbol = logical_bits,
            .leading_symbol_offset = static_cast<std::uint16_t>(leading),
            .trailing_padding = static_cast<std::uint16_t>(trailing),
            .bit_order = view_type::packed_symbol_order::
                least_significant_symbol_first,
        };
    }
}

template <class R, class Scanner>
[[nodiscard]] bool owned_sequence_copy_packed(
        void const* state,
        void* destination,
        std::size_t unit_count,
        std::uint8_t requested_width) {
    using range_traits = typename compact_sequence_range<R>::traits;
    using storage_unit = typename range_traits::storage_unit;
    using alphabet_type = typename range_traits::alphabet_type;
    if constexpr (requires (
            const Scanner& scanner,
            std::span<storage_unit> output) {
                { scanner.copy_packed_to(output) } -> std::same_as<void>;
            }) {
        constexpr std::uint8_t physical_width =
            static_cast<std::uint8_t>(sizeof(storage_unit));
        constexpr std::size_t logical_bits =
            lazy_sequence_traits<alphabet_type>::logical_bits_per_symbol;
        constexpr std::size_t letters_per_unit = range_traits::letters_per_unit;
        const auto& sequence =
            *static_cast<const lazy_sequence<R, Scanner>*>(state);
        const sequence_size_t expected_units = bioseq::common::ceil_div(
            sequence.bp_size(),
            static_cast<sequence_size_t>(letters_per_unit));
        if (requested_width != physical_width
            || range_traits::bits_per_char != logical_bits
            || expected_units
                    > static_cast<sequence_size_t>(
                        (std::numeric_limits<std::size_t>::max)())
            || unit_count != static_cast<std::size_t>(expected_units)
            || (unit_count != 0u && destination == nullptr)
            || (unit_count != 0u
                && reinterpret_cast<std::uintptr_t>(destination)
                        % alignof(storage_unit)
                    != 0u)) {
            return false;
        }
        sequence.scanner().copy_packed_to(
            std::span<storage_unit>{
                static_cast<storage_unit*>(destination), unit_count});
        return true;
    } else {
        (void)state;
        (void)destination;
        (void)unit_count;
        (void)requested_width;
        return false;
    }
}

template <class R, class Scanner>
struct owned_sequence_adapter {
    using alphabet_type =
        typename compact_sequence_range<R>::traits::alphabet_type;
    using view_type = lazy_sequence_view<alphabet_type>;

    static inline const typename view_type::backend_ops ops{
        .bp_size = &owned_sequence_bp_size<R, Scanner>,
        .read_at = {
            &owned_sequence_read_at<false, false, R, Scanner>,
            &owned_sequence_read_at<false, true, R, Scanner>,
            &owned_sequence_read_at<true, false, R, Scanner>,
            &owned_sequence_read_at<true, true, R, Scanner>,
        },
        .try_resident_packed = &owned_sequence_resident_probe<R, Scanner>,
        .copy_packed_sequence = &owned_sequence_copy_packed<R, Scanner>,
        .worker_shareable = &owned_sequence_worker_shareable<R, Scanner>,
    };
};

// Inline compact ranges are already stable owners. Adapt them directly so
// callers do not need to name or retain the concrete no-fragment scanner
// wrapper merely to cross the canonical facade boundary. Each data callback
// creates the pointer-sized wrapper locally; encoded bytes are copied into the
// caller scratch before it returns, while a resident probe points into the
// range's own storage.
template <bool IsReversed, bool WriteReversed, class R>
[[nodiscard]] const std::uint8_t* compact_range_read_at(
        void const* state,
        sequence_size_t bp_from,
        std::size_t required_length,
        std::span<std::uint8_t> scratch) {
    using scanner_type = resident_sequence_scanner<
        typename compact_sequence_range<R>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<R>*>(state);
    const lazy_sequence<R, scanner_type> owner{range};
    return owned_sequence_read_at<IsReversed, WriteReversed, R, scanner_type>(
        std::addressof(owner), bp_from, required_length, scratch);
}

template <class R>
[[nodiscard]] sequence_size_t compact_range_bp_size(
        void const* state) noexcept {
    using scanner_type = resident_sequence_scanner<
        typename compact_sequence_range<R>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<R>*>(state);
    const lazy_sequence<R, scanner_type> owner{range};
    return owner.bp_size();
}

template <class R>
[[nodiscard]] bool compact_range_worker_shareable(void const*) noexcept {
    return true;
}

template <class R>
[[nodiscard]] auto compact_range_resident_probe(
        void const* state,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width) noexcept
    -> typename lazy_sequence_view<
        typename compact_sequence_range<R>::traits::alphabet_type>::
        resident_packed_result {
    using scanner_type = resident_sequence_scanner<
        typename compact_sequence_range<R>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<R>*>(state);
    const lazy_sequence<R, scanner_type> owner{range};
    return owned_sequence_resident_probe<R, scanner_type>(
        std::addressof(owner), from, to, requested_width);
}

template <class R>
struct compact_range_adapter {
    using alphabet_type =
        typename compact_sequence_range<R>::traits::alphabet_type;
    using view_type = lazy_sequence_view<alphabet_type>;

    static inline const typename view_type::backend_ops ops{
        .bp_size = &compact_range_bp_size<R>,
        .read_at = {
            &compact_range_read_at<false, false, R>,
            &compact_range_read_at<false, true, R>,
            &compact_range_read_at<true, false, R>,
            &compact_range_read_at<true, true, R>,
        },
        .try_resident_packed = &compact_range_resident_probe<R>,
        .copy_packed_sequence = nullptr,
        .worker_shareable = &compact_range_worker_shareable<R>,
    };
};

}  // namespace lazy_sequence_detail

template <class R, class Scanner>
[[nodiscard]] inline auto make_lazy_sequence_view(
        const lazy_sequence<R, Scanner>& owner) noexcept {
    using adapter = lazy_sequence_detail::owned_sequence_adapter<R, Scanner>;
    return typename adapter::view_type{
        std::addressof(owner), std::addressof(adapter::ops)};
}

template <class R>
[[nodiscard]] inline auto make_lazy_sequence_view(
        const compact_sequence_range<R>& owner) noexcept {
    using adapter = lazy_sequence_detail::compact_range_adapter<R>;
    return typename adapter::view_type{
        std::addressof(owner), std::addressof(adapter::ops)};
}

/**
 * Invoke a callback through a synthetic fragmented scanner. The concrete
 * scanner and lazy owner remain inside the storage module; callers receive
 * only the canonical facade by value and cannot accidentally flatten the
 * access route.
 */
template <std::size_t FragmentStorageUnits =
              default_fragment_storage_units,
          class R, class Fn>
inline void with_chunked_lazy_sequence(
        const compact_sequence_range<R>& range,
        Fn&& fn) {
    using scanner_type = chunked_span_sequence_scanner<
        typename compact_sequence_range<R>::traits::storage_unit,
        FragmentStorageUnits>;
    lazy_sequence<R, scanner_type> owner{
        scanner_type::from_range(range)};
    std::forward<Fn>(fn)(make_lazy_sequence_view(owner));
}

/** Keep two synthetic fragmented owners alive for one facade-only callback. */
template <std::size_t FragmentStorageUnits =
              default_fragment_storage_units,
          class LeftR, class RightR, class Fn>
inline void with_chunked_lazy_sequence(
        const compact_sequence_range<LeftR>& left_range,
        const compact_sequence_range<RightR>& right_range,
        Fn&& fn) {
    using left_alphabet =
        typename compact_sequence_range<LeftR>::traits::alphabet_type;
    using right_alphabet =
        typename compact_sequence_range<RightR>::traits::alphabet_type;
    static_assert(std::is_same_v<left_alphabet, right_alphabet>,
                  "chunked facade pair must share one alphabet");
    with_chunked_lazy_sequence<FragmentStorageUnits>(
        left_range,
        [&right_range, &fn](auto left_view) {
            with_chunked_lazy_sequence<FragmentStorageUnits>(
                right_range,
                [left_view, &fn](auto right_view) {
                    std::forward<Fn>(fn)(left_view, right_view);
                });
        });
}

template <sequence_transform Transform,
          sequence_alphabet Alphabet,
          class OutputWriter>
void stream_transformed_ranks(
        lazy_sequence_view<Alphabet> view,
        std::uint64_t from,
        std::uint64_t to,
        OutputWriter& output) {
    if (from > to || to > view.bp_size()
        || to > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        throw std::invalid_argument(
            "transformed sequence stream coordinates are unsupported");
    }
    std::array<std::uint8_t, 256> scratch{};
    auto append = [&output](std::span<const std::uint8_t> ranks) {
        if constexpr (std::same_as<
                          decltype(output.append_ranks(ranks)),
                          bioseq::sequences::rank_pack_status>) {
            return output.append_ranks(ranks)
                == bioseq::sequences::rank_pack_status::ok;
        } else if constexpr (std::same_as<
                                 decltype(output.append_ranks(ranks)), bool>) {
            return output.append_ranks(ranks);
        } else {
            output.append_ranks(ranks);
            return true;
        }
    };

    if constexpr (is_reversed(Transform)) {
        std::uint64_t position = to;
        while (position > from) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(scratch.size(), position - from));
            const std::uint64_t chunk_from = position - count;
            view.template copy_ranks<false, true>(
                std::span<std::uint8_t>{scratch.data(), count},
                static_cast<std::size_t>(chunk_from),
                static_cast<std::size_t>(position));
            if constexpr (is_complemented(Transform)) {
                static_assert(
                    lazy_sequence_detail::complementable_sequence_alphabet<
                        Alphabet>);
                for (std::size_t index = 0u; index < count; ++index) {
                    scratch[index] = lazy_sequence_traits<Alphabet>::
                        letter_complement(scratch[index]);
                }
            }
            if (!append(
                    std::span<const std::uint8_t>{scratch.data(), count})) {
                return;
            }
            position = chunk_from;
        }
    } else {
        std::uint64_t position = from;
        while (position < to) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(scratch.size(), to - position));
            view.template copy_ranks<false, false>(
                std::span<std::uint8_t>{scratch.data(), count},
                static_cast<std::size_t>(position),
                static_cast<std::size_t>(position + count));
            if constexpr (is_complemented(Transform)) {
                static_assert(
                    lazy_sequence_detail::complementable_sequence_alphabet<
                        Alphabet>);
                for (std::size_t index = 0u; index < count; ++index) {
                    scratch[index] = lazy_sequence_traits<Alphabet>::
                        letter_complement(scratch[index]);
                }
            }
            if (!append(
                    std::span<const std::uint8_t>{scratch.data(), count})) {
                return;
            }
            position += count;
        }
    }
}

template <sequence_alphabet Alphabet>
[[nodiscard]] int compare_lazy_sequences(
        lazy_sequence_view<Alphabet> left,
        lazy_sequence_view<Alphabet> right) {
    struct comparison_writer {
        typename lazy_sequence_view<Alphabet>::rank_iterator cursor;
        typename lazy_sequence_view<Alphabet>::rank_iterator end;
        int result{};

        bool append_ranks(std::span<const std::uint8_t> ranks) {
            if (result != 0) {
                return false;
            }
            for (const std::uint8_t rank : ranks) {
                if (cursor == end) {
                    result = 1;
                    return false;
                }
                const std::uint8_t other = *cursor;
                if (rank < other) {
                    result = -1;
                    return false;
                }
                if (rank > other) {
                    result = 1;
                    return false;
                }
                ++cursor;
            }
            return true;
        }
    } writer{right.begin(), right.end(), 0};
    stream_transformed_ranks<sequence_transform::identity>(
        left, 0u, left.bp_size(), writer);
    if (writer.result != 0) {
        return writer.result;
    }
    return writer.cursor == writer.end ? 0 : -1;
}

namespace lazy_sequence_detail {

struct logical_hash_writer {
    static constexpr std::uint64_t fnv_offset_basis =
        14695981039346656037ull;
    static constexpr std::uint64_t fnv_prime = 1099511628211ull;

    explicit logical_hash_writer(std::uint64_t seed) noexcept
        : hash(fnv_offset_basis ^ seed) {}

    void append_ranks(std::span<const std::uint8_t> ranks) noexcept {
        for (const std::uint8_t rank : ranks) {
            hash ^= rank;
            hash *= fnv_prime;
        }
    }

    [[nodiscard]] std::uint64_t finish(
            std::uint64_t logical_length) noexcept {
        for (unsigned shift = 0u; shift < 64u; shift += 8u) {
            hash ^= static_cast<std::uint8_t>(logical_length >> shift);
            hash *= fnv_prime;
        }
        hash ^= hash >> 30u;
        hash *= 0xbf58476d1ce4e5b9ull;
        hash ^= hash >> 27u;
        hash *= 0x94d049bb133111ebull;
        hash ^= hash >> 31u;
        return hash;
    }

    std::uint64_t hash;
};

}  // namespace lazy_sequence_detail

template <std::ranges::input_range RankRange>
[[nodiscard]] std::uint64_t hash_logical_ranks(
        RankRange&& ranks,
        std::uint64_t logical_length,
        std::uint64_t seed) {
    lazy_sequence_detail::logical_hash_writer writer{seed};
    for (auto&& rank : ranks) {
        const std::uint8_t value = static_cast<std::uint8_t>(rank);
        writer.append_ranks(std::span<const std::uint8_t>{&value, 1u});
    }
    return writer.finish(logical_length);
}

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint64_t hash_logical_sequence(
        lazy_sequence_view<Alphabet> view,
        std::uint64_t seed) {
    lazy_sequence_detail::logical_hash_writer writer{seed};
    stream_transformed_ranks<sequence_transform::identity>(
        view, 0u, view.bp_size(), writer);
    return writer.finish(view.bp_size());
}

namespace lazy_sequence_detail {

class aligned_packed_storage {
public:
    aligned_packed_storage() = default;
    aligned_packed_storage(aligned_packed_storage const&) = delete;
    aligned_packed_storage& operator=(aligned_packed_storage const&) = delete;
    aligned_packed_storage(aligned_packed_storage&&) noexcept = default;
    aligned_packed_storage& operator=(aligned_packed_storage&&) noexcept = default;

    [[nodiscard]] static aligned_packed_storage allocate(std::size_t bytes) {
        aligned_packed_storage storage;
        if (bytes == 0u) {
            return storage;
        }
        storage.data_ = hwy::AllocateAligned<std::uint8_t>(bytes);
        if (storage.data_ == nullptr) {
            throw std::bad_alloc{};
        }
        storage.size_ = bytes;
        std::memset(storage.data_.get(), 0, bytes);
        return storage;
    }

    [[nodiscard]] std::uint8_t* data() noexcept { return data_.get(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return data_.get();
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    hwy::AlignedFreeUniquePtr<std::uint8_t[]> data_{};
    std::size_t size_{};
};

template <sequence_alphabet Alphabet>
struct resident_view_ops_context {
    using descriptor_type =
        typename lazy_sequence_view<Alphabet>::resident_packed_sequence;
    descriptor_type const* descriptor{};
};

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint64_t load_resident_unit(
        resident_view_ops_context<Alphabet> const& context,
        std::size_t unit_index) noexcept {
    const auto& descriptor = *context.descriptor;
    const auto* bytes = static_cast<const std::uint8_t*>(descriptor.units)
        + unit_index * descriptor.storage_unit_width;
    switch (descriptor.storage_unit_width) {
        case 1u: {
            std::uint8_t value{};
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }
        case 2u: {
            std::uint16_t value{};
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }
        case 4u: {
            std::uint32_t value{};
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }
        case 8u: {
            std::uint64_t value{};
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }
        default:
            return 0u;
    }
}

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint8_t resident_rank_at(
        resident_view_ops_context<Alphabet> const& context,
        sequence_size_t position) noexcept {
    const auto& descriptor = *context.descriptor;
    const sequence_size_t letters_per_unit =
        descriptor.storage_unit_width * CHAR_BIT
        / descriptor.bits_per_symbol;
    const sequence_size_t slot =
        descriptor.leading_symbol_offset + position;
    const std::size_t unit_index = static_cast<std::size_t>(
        slot / letters_per_unit);
    const auto shift = static_cast<unsigned>(
        (slot % letters_per_unit) * descriptor.bits_per_symbol);
    const std::uint64_t mask =
        (std::uint64_t{1} << descriptor.bits_per_symbol) - 1u;
    return static_cast<std::uint8_t>(
        (load_resident_unit(context, unit_index) >> shift) & mask);
}

template <bool IsReversed, bool WriteReversed, sequence_alphabet Alphabet>
[[nodiscard]] const std::uint8_t* resident_read_at(
        void const* state,
        sequence_size_t bp_from,
        std::size_t required_length,
        std::span<std::uint8_t> scratch) {
    const auto& context =
        *static_cast<const resident_view_ops_context<Alphabet>*>(state);
    const sequence_size_t total = context.descriptor->logical_length;
    if (bp_from > total
        || required_length > total - bp_from
        || scratch.size() < required_length) {
        abort_lazy_sequence_access(
            "resident packed sequence rank range is invalid");
    }
    if (required_length == 0u) {
        return scratch.data();
    }
    if constexpr (IsReversed
                  && !complementable_sequence_alphabet<Alphabet>) {
        throw std::invalid_argument(
            "reverse-complement encoding requires a complementable alphabet");
    }
    const sequence_size_t forward_from = IsReversed
        ? total - bp_from - required_length
        : bp_from;
    constexpr bool descending_write =
        (IsReversed && !WriteReversed) || (!IsReversed && WriteReversed);
    for (std::size_t index = 0u; index < required_length; ++index) {
        std::uint8_t rank = resident_rank_at(
            context, forward_from + index);
        if constexpr (IsReversed
                      && complementable_sequence_alphabet<Alphabet>) {
            rank = lazy_sequence_traits<Alphabet>::letter_complement(rank);
        }
        scratch[descending_write ? required_length - 1u - index : index] = rank;
    }
    return scratch.data();
}

template <sequence_alphabet Alphabet>
[[nodiscard]] sequence_size_t resident_bp_size(void const* state) noexcept {
    const auto& context =
        *static_cast<const resident_view_ops_context<Alphabet>*>(state);
    return context.descriptor->logical_length;
}

template <sequence_alphabet Alphabet>
[[nodiscard]] auto resident_probe(
        void const* state,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width) noexcept
    -> typename lazy_sequence_view<Alphabet>::resident_packed_result {
    using view_type = lazy_sequence_view<Alphabet>;
    using descriptor_type = typename view_type::resident_packed_sequence;
    const auto& context =
        *static_cast<const resident_view_ops_context<Alphabet>*>(state);
    const descriptor_type& source = *context.descriptor;
    if (from > to || to > source.logical_length
        || requested_width != source.storage_unit_width) {
        return std::nullopt;
    }
    const sequence_size_t length = to - from;
    if (length == 0u) {
        return descriptor_type{
            .units = nullptr,
            .unit_count = 0u,
            .logical_length = 0u,
            .storage_unit_width = source.storage_unit_width,
            .bits_per_symbol = source.bits_per_symbol,
            .leading_symbol_offset = 0u,
            .trailing_padding = 0u,
            .bit_order = source.bit_order,
        };
    }

    const sequence_size_t letters_per_unit =
        source.storage_unit_width * CHAR_BIT / source.bits_per_symbol;
    const auto absolute_slot = bioseq::common::checked_add(
        static_cast<sequence_size_t>(source.leading_symbol_offset), from);
    if (!absolute_slot.has_value()) {
        return std::nullopt;
    }
    const sequence_size_t first_unit = *absolute_slot / letters_per_unit;
    const sequence_size_t leading = *absolute_slot % letters_per_unit;
    const auto occupied = bioseq::common::checked_add(leading, length);
    if (!occupied.has_value()) {
        return std::nullopt;
    }
    const sequence_size_t unit_count = bioseq::common::ceil_div(
        *occupied, letters_per_unit);
    const auto represented_symbols = bioseq::common::checked_mul(
        unit_count, letters_per_unit);
    if (first_unit > source.unit_count
        || unit_count > source.unit_count - first_unit
        || !represented_symbols.has_value()
        || unit_count
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
        return std::nullopt;
    }
    const auto* units = static_cast<const std::uint8_t*>(source.units)
        + first_unit * source.storage_unit_width;
    return descriptor_type{
        .units = units,
        .unit_count = static_cast<std::size_t>(unit_count),
        .logical_length = length,
        .storage_unit_width = source.storage_unit_width,
        .bits_per_symbol = source.bits_per_symbol,
        .leading_symbol_offset = static_cast<std::uint16_t>(leading),
        .trailing_padding = static_cast<std::uint16_t>(
            *represented_symbols - *occupied),
        .bit_order = source.bit_order,
    };
}

[[nodiscard]] inline bool resident_worker_shareable(void const*) noexcept {
    return true;
}

template <sequence_alphabet Alphabet>
struct resident_adapter {
    using view_type = lazy_sequence_view<Alphabet>;
    static inline const typename view_type::backend_ops ops{
        .bp_size = &resident_bp_size<Alphabet>,
        .read_at = {
            &resident_read_at<false, false, Alphabet>,
            &resident_read_at<false, true, Alphabet>,
            &resident_read_at<true, false, Alphabet>,
            &resident_read_at<true, true, Alphabet>,
        },
        .try_resident_packed = &resident_probe<Alphabet>,
        .copy_packed_sequence = nullptr,
        .worker_shareable = &resident_worker_shareable,
    };
};

template <sequence_alphabet Alphabet, class Unit>
[[nodiscard]] bioseq::sequences::rank_pack_result pack_view_region_into(
        lazy_sequence_view<Alphabet> view,
        sequence_size_t from,
        sequence_size_t to,
        void* output,
        std::size_t unit_count) {
    constexpr std::size_t bits =
        lazy_sequence_traits<Alphabet>::logical_bits_per_symbol;
    using output_instance = sequence_custom_alphabet<Alphabet, Unit, bits>;
    using unit_sink = bioseq::sequences::fixed_rank_pack_unit_sink<Unit>;
    using packer = bioseq::sequences::checked_rank_pack_sink<
        output_instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        unit_sink>;

    auto units = std::span<Unit>(static_cast<Unit*>(output), unit_count);
    packer writer{unit_sink{units}};
    stream_transformed_ranks<sequence_transform::identity>(
        view, from, to, writer);
    return writer.finish(to - from, unit_count);
}

}  // namespace lazy_sequence_detail

template <sequence_alphabet Alphabet>
class admitted_packed_sequence;

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint64_t packed_sequence_admission_bytes(
    sequence_size_t logical_length,
    std::uint8_t requested_width);

template <sequence_alphabet Alphabet>
[[nodiscard]] admitted_packed_sequence<Alphabet> materialize_packed_sequence(
    lazy_sequence_view<Alphabet> view,
    sequence_size_t from,
    sequence_size_t to,
    std::uint8_t requested_width,
    bioseq::parallelism::memory_permit&& permit);

template <sequence_alphabet Alphabet>
class admitted_packed_sequence {
public:
    using view_type = lazy_sequence_view<Alphabet>;
    using descriptor_type = typename view_type::resident_packed_sequence;

    admitted_packed_sequence(admitted_packed_sequence&&) noexcept = default;
    admitted_packed_sequence& operator=(admitted_packed_sequence&&) = delete;
    admitted_packed_sequence(admitted_packed_sequence const&) = delete;
    admitted_packed_sequence& operator=(admitted_packed_sequence const&) = delete;

    [[nodiscard]] descriptor_type const& descriptor() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("access to moved admitted packed region");
        }
        return state_->descriptor_;
    }

    [[nodiscard]] view_type view() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("view of moved admitted packed region");
        }
        using adapter = lazy_sequence_detail::resident_adapter<Alphabet>;
        return view_type{std::addressof(state_->view_context_),
                         std::addressof(adapter::ops)};
    }

#ifdef BIOSEQ_TEST_INDEXES
    [[nodiscard]] std::uint64_t test_admission_bytes() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("admission query on moved packed region");
        }
        return state_->permit_.bytes();
    }

    [[nodiscard]] std::uintptr_t test_storage_address() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("storage query on moved packed region");
        }
        return reinterpret_cast<std::uintptr_t>(state_->descriptor_.units);
    }
#endif

private:
    struct stable_state {
        bioseq::parallelism::memory_permit permit_;
        lazy_sequence_detail::aligned_packed_storage units_;
        descriptor_type descriptor_;
        lazy_sequence_detail::resident_view_ops_context<Alphabet> view_context_;

        stable_state(bioseq::parallelism::memory_permit&& permit,
                     std::size_t allocation_bytes,
                     descriptor_type descriptor)
            : permit_(std::move(permit)),
              units_(lazy_sequence_detail::aligned_packed_storage::allocate(
                  allocation_bytes)),
              descriptor_(descriptor),
              view_context_{std::addressof(descriptor_)} {
            descriptor_.units = units_.data();
        }
    };

    explicit admitted_packed_sequence(std::unique_ptr<stable_state> state) noexcept
        : state_(std::move(state)) {}

    template <sequence_alphabet OtherAlphabet>
    friend admitted_packed_sequence<OtherAlphabet> materialize_packed_sequence(
        lazy_sequence_view<OtherAlphabet>,
        sequence_size_t,
        sequence_size_t,
        std::uint8_t,
        bioseq::parallelism::memory_permit&&);

    template <sequence_alphabet OtherAlphabet>
    friend std::uint64_t packed_sequence_admission_bytes(
        sequence_size_t,
        std::uint8_t);

    std::unique_ptr<stable_state> state_;
};

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint64_t packed_sequence_admission_bytes(
        sequence_size_t logical_length,
        std::uint8_t requested_width) {
    using owner_type = admitted_packed_sequence<Alphabet>;
    if (!(requested_width == 1u || requested_width == 2u
          || requested_width == 4u || requested_width == 8u)) {
        throw std::invalid_argument("invalid packed storage-unit width");
    }
    const sequence_size_t letters_per_unit =
        packed_letters_per_unit(
            requested_width,
            static_cast<std::uint8_t>(
                lazy_sequence_traits<Alphabet>::logical_bits_per_symbol));
    const sequence_size_t unit_count = bioseq::common::ceil_div(
        logical_length, letters_per_unit);
    const auto allocation_bytes = bioseq::common::checked_mul(
        unit_count, static_cast<sequence_size_t>(requested_width));
    if (!allocation_bytes.has_value()
        || unit_count > static_cast<sequence_size_t>(
            (std::numeric_limits<std::size_t>::max)())
        || *allocation_bytes > static_cast<sequence_size_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error("packed materialization size overflow");
    }
    const auto admitted_bytes = bioseq::common::checked_add(
        *allocation_bytes,
        static_cast<std::uint64_t>(
            sizeof(typename owner_type::stable_state)));
    if (!admitted_bytes.has_value()) {
        throw std::overflow_error("packed materialization admission overflow");
    }
    return *admitted_bytes;
}

enum class sequence_access_route : std::uint8_t {
    stream_symbols,
    borrow_resident,
    materialize_admitted,
};

/// Controls what a contiguous-resident consumer is permitted to borrow.
/// CPU algorithms that form or dereference `storage_unit*` must keep the
/// default.  The byte-upload policy is intentionally narrow: it permits a
/// layout-compatible cross-width descriptor only for consumers that copy the
/// object representation as bytes and never form typed loads from it.
enum class sequence_resident_borrow_policy : std::uint8_t {
    native_typed_storage_only,
    byte_upload_compatible,
};

/// Truthful receipt for the storage behind a borrowed descriptor.  In
/// particular, `reinterpreted_bytes` is not typed storage of requested_width
/// and must never be passed to a CPU `compact_sequence_range` consumer.
enum class sequence_resident_borrow_kind : std::uint8_t {
    none,
    native_typed_storage,
    reinterpreted_bytes,
};

template <sequence_alphabet Alphabet>
struct sequence_access_plan {
    using descriptor_type = typename lazy_sequence_view<Alphabet>::
        resident_packed_sequence;

    sequence_access_requirement requirement{
        sequence_access_requirement::sequential_symbols};
    sequence_access_route route{sequence_access_route::stream_symbols};
    std::optional<descriptor_type> resident{};
    sequence_resident_borrow_kind resident_borrow_kind{
        sequence_resident_borrow_kind::none};
    std::uint64_t admission_bytes{};
    sequence_size_t from{};
    sequence_size_t to{};
    std::uint8_t requested_width{};
};

/// Borrow a narrower packed *byte representation* over already resident
/// storage.  The only cross-width shape required by current upload paths is
/// canonical u64 -> u32 DNA packing.  On little-endian hosts the low u32 word
/// of each u64 contains the earlier LSB-first symbols, so splitting each
/// resident word is byte-for-byte identical to the established packed wire
/// layout.
///
/// The returned descriptor does not change the effective type of the source
/// allocation.  Its `units` member may be copied/read only through
/// character/byte access (for example a Vulkan buffer upload); dereferencing
/// it as `uint32_t*` would violate the C++ effective-type/aliasing rules.
template <sequence_alphabet Alphabet>
[[nodiscard]] std::optional<
    typename lazy_sequence_view<Alphabet>::resident_packed_sequence>
try_borrow_reinterpreted_packed_bytes(
        lazy_sequence_view<Alphabet> view,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width) noexcept {
    using view_type = lazy_sequence_view<Alphabet>;
    using descriptor_type = typename view_type::resident_packed_sequence;
    if constexpr (std::endian::native != std::endian::little) {
        return std::nullopt;
    } else {
        constexpr std::uint8_t source_width = sizeof(std::uint64_t);
        constexpr std::uint8_t target_width = sizeof(std::uint32_t);
        constexpr std::uint8_t bits = static_cast<std::uint8_t>(
            lazy_sequence_traits<Alphabet>::logical_bits_per_symbol);
        if (requested_width != target_width || from > to
            || to > view.bp_size()) {
            return std::nullopt;
        }
        if (bits == 0u
            || (source_width * CHAR_BIT) % bits != 0u
            || (target_width * CHAR_BIT) % bits != 0u) {
            return std::nullopt;
        }
        const sequence_size_t target_letters_per_unit =
            packed_letters_per_unit(target_width, bits);
        if (target_letters_per_unit == 0u) {
            return std::nullopt;
        }
        auto source = view.try_resident_packed(from, to, source_width);
        if (!source.has_value()
            || source->bit_order
                != view_type::packed_symbol_order::
                    least_significant_symbol_first
            || source->bits_per_symbol != bits
            || source->leading_symbol_offset % target_letters_per_unit != 0u) {
            return std::nullopt;
        }
        const sequence_size_t length = to - from;
        if (length == 0u) {
            return descriptor_type{
                .units = nullptr,
                .unit_count = 0u,
                .logical_length = 0u,
                .storage_unit_width = target_width,
                .bits_per_symbol = bits,
                .leading_symbol_offset = 0u,
                .trailing_padding = 0u,
                .bit_order = view_type::packed_symbol_order::
                    least_significant_symbol_first,
            };
        }
        const sequence_size_t target_units_u64 = bioseq::common::ceil_div(
            length, target_letters_per_unit);
        const auto represented_symbols = bioseq::common::checked_mul(
            target_units_u64, target_letters_per_unit);
        if (target_units_u64
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || !represented_symbols.has_value()) {
            return std::nullopt;
        }
        const std::size_t halfword_offset = static_cast<std::size_t>(
            source->leading_symbol_offset / target_letters_per_unit);
        const std::size_t target_units =
            static_cast<std::size_t>(target_units_u64);
        const auto source_bytes = bioseq::common::checked_mul(
            source->unit_count,
            static_cast<std::size_t>(source_width));
        const auto byte_offset = bioseq::common::checked_mul(
            halfword_offset,
            static_cast<std::size_t>(target_width));
        const auto target_bytes = bioseq::common::checked_mul(
            target_units,
            static_cast<std::size_t>(target_width));
        if (!source_bytes.has_value()
            || !byte_offset.has_value()
            || !target_bytes.has_value()) {
            return std::nullopt;
        }
        if (*byte_offset > *source_bytes
            || *target_bytes > *source_bytes - *byte_offset) {
            return std::nullopt;
        }
        const sequence_size_t trailing_u64 =
            *represented_symbols - length;
        if (trailing_u64
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::uint16_t>::max)())) {
            return std::nullopt;
        }
        const auto* source_bytes_ptr = static_cast<const std::uint8_t*>(
            source->units);
        const void* target_units_ptr = source_bytes_ptr + *byte_offset;
        if (reinterpret_cast<std::uintptr_t>(target_units_ptr)
                % target_width != 0u) {
            return std::nullopt;
        }
        return descriptor_type{
            .units = target_units_ptr,
            .unit_count = target_units,
            .logical_length = length,
            .storage_unit_width = target_width,
            .bits_per_symbol = bits,
            .leading_symbol_offset = 0u,
            .trailing_padding =
                static_cast<std::uint16_t>(trailing_u64),
            .bit_order = view_type::packed_symbol_order::
                least_significant_symbol_first,
        };
    }
}

template <sequence_alphabet Alphabet>
[[nodiscard]] sequence_access_plan<Alphabet> plan_sequence_access(
        lazy_sequence_view<Alphabet> view,
        sequence_access_requirement requirement,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width = 0u,
        sequence_resident_borrow_policy borrow_policy =
            sequence_resident_borrow_policy::native_typed_storage_only) {
    if (from > to || to > view.bp_size()) {
        throw std::invalid_argument("invalid sequence access range");
    }
    sequence_access_plan<Alphabet> plan{
        .requirement = requirement,
        .route = sequence_access_route::stream_symbols,
        .resident = std::nullopt,
        .resident_borrow_kind = sequence_resident_borrow_kind::none,
        .admission_bytes = 0u,
        .from = from,
        .to = to,
        .requested_width = requested_width,
    };
    if (requirement == sequence_access_requirement::sequential_symbols) {
        return plan;
    }
    if (!(requested_width == 1u || requested_width == 2u
          || requested_width == 4u || requested_width == 8u)) {
        throw std::invalid_argument(
            "resident sequence access requires width 1, 2, 4, or 8");
    }
    if (requirement == sequence_access_requirement::contiguous_resident) {
        plan.resident = view.try_resident_packed(
            from, to, requested_width);
        if (plan.resident.has_value()) {
            plan.resident_borrow_kind =
                sequence_resident_borrow_kind::native_typed_storage;
        } else if (borrow_policy
                == sequence_resident_borrow_policy::byte_upload_compatible) {
            plan.resident = try_borrow_reinterpreted_packed_bytes(
                view, from, to, requested_width);
            if (plan.resident.has_value()) {
                plan.resident_borrow_kind =
                    sequence_resident_borrow_kind::reinterpreted_bytes;
            }
        }
        if (plan.resident.has_value()
            && plan.resident->leading_symbol_offset == 0u) {
            plan.route = sequence_access_route::borrow_resident;
            return plan;
        }
        plan.resident.reset();
        plan.resident_borrow_kind = sequence_resident_borrow_kind::none;
    }
    plan.route = sequence_access_route::materialize_admitted;
    plan.admission_bytes = packed_sequence_admission_bytes<Alphabet>(
        to - from, requested_width);
    return plan;
}

template <sequence_alphabet Alphabet>
[[nodiscard]] admitted_packed_sequence<Alphabet> materialize_packed_sequence(
        lazy_sequence_view<Alphabet> view,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width,
        bioseq::parallelism::memory_permit&& permit) {
    using owner_type = admitted_packed_sequence<Alphabet>;
    using descriptor_type = typename owner_type::descriptor_type;
    using bit_order_type =
        typename lazy_sequence_view<Alphabet>::packed_symbol_order;
    constexpr std::uint8_t bits = static_cast<std::uint8_t>(
        lazy_sequence_traits<Alphabet>::logical_bits_per_symbol);

    if (from > to || to > view.bp_size()) {
        throw std::invalid_argument("invalid lazy packed materialization range");
    }
    if (!(requested_width == 1u || requested_width == 2u
          || requested_width == 4u || requested_width == 8u)) {
        throw std::invalid_argument("invalid packed storage-unit width");
    }
    if (to > static_cast<sequence_size_t>(
                 (std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error("packed materialization coordinate overflow");
    }

    const sequence_size_t length = to - from;
    const sequence_size_t letters_per_unit =
        packed_letters_per_unit(requested_width, bits);
    const sequence_size_t unit_count = bioseq::common::ceil_div(
        length, letters_per_unit);
    const auto allocation_bytes_u64 = bioseq::common::checked_mul(
        unit_count, static_cast<sequence_size_t>(requested_width));
    const auto represented_symbols = bioseq::common::checked_mul(
        unit_count, letters_per_unit);
    if (!allocation_bytes_u64.has_value()
        || !represented_symbols.has_value()
        || unit_count > static_cast<sequence_size_t>(
            (std::numeric_limits<std::size_t>::max)())
        || *allocation_bytes_u64 > static_cast<sequence_size_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error("packed materialization size overflow");
    }
    const std::size_t allocation_bytes =
        static_cast<std::size_t>(*allocation_bytes_u64);
    const std::uint64_t admitted_bytes = packed_sequence_admission_bytes<Alphabet>(
        length, requested_width);
    if (!permit || permit.bytes() < admitted_bytes) {
        throw std::length_error(
            "packed materialization permit is smaller than retained storage");
    }

    const sequence_size_t trailing =
        *represented_symbols - length;
    descriptor_type descriptor{
        .units = nullptr,
        .unit_count = static_cast<std::size_t>(unit_count),
        .logical_length = length,
        .storage_unit_width = requested_width,
        .bits_per_symbol = bits,
        .leading_symbol_offset = 0u,
        .trailing_padding = static_cast<std::uint16_t>(trailing),
        .bit_order = bit_order_type::least_significant_symbol_first,
    };
    auto state = std::make_unique<typename owner_type::stable_state>(
        std::move(permit), allocation_bytes, descriptor);

    const bool bulk_copied = from == 0u && to == view.bp_size()
        && view.try_copy_packed_sequence(
            std::span<std::uint8_t>{
                state->units_.data(), allocation_bytes},
            static_cast<std::size_t>(unit_count), requested_width);
    if (!bulk_copied) {
        bioseq::sequences::rank_pack_result packed{};
        switch (requested_width) {
            case 1u:
                packed = lazy_sequence_detail::pack_view_region_into<
                    Alphabet, std::uint8_t>(
                        view, from, to, state->units_.data(),
                        static_cast<std::size_t>(unit_count));
                break;
            case 2u:
                packed = lazy_sequence_detail::pack_view_region_into<
                    Alphabet, std::uint16_t>(
                        view, from, to, state->units_.data(),
                        static_cast<std::size_t>(unit_count));
                break;
            case 4u:
                packed = lazy_sequence_detail::pack_view_region_into<
                    Alphabet, std::uint32_t>(
                        view, from, to, state->units_.data(),
                        static_cast<std::size_t>(unit_count));
                break;
            case 8u:
                packed = lazy_sequence_detail::pack_view_region_into<
                    Alphabet, std::uint64_t>(
                        view, from, to, state->units_.data(),
                        static_cast<std::size_t>(unit_count));
                break;
            default:
                throw std::invalid_argument(
                    "invalid packed storage-unit width");
        }
        if (packed.status != bioseq::sequences::rank_pack_status::ok
            || packed.emitted_symbols != length
            || packed.written_units != unit_count
            || packed.padding_letters != trailing) {
            throw std::runtime_error("checked lazy rank packing failed");
        }
    }
    (void)publish_sequence_allocation(
        view.payload_allocation_observer(),
        sequence_payload_allocation_kind::admitted_packed_sequence,
        length, length, static_cast<std::uint64_t>(allocation_bytes),
        admitted_bytes);
    return owner_type{std::move(state)};
}

static_assert(std::is_move_constructible_v<
              admitted_packed_sequence<seq::dna4>>);
static_assert(!std::is_move_assignable_v<
              admitted_packed_sequence<seq::dna4>>);
static_assert(!std::is_copy_constructible_v<
              admitted_packed_sequence<seq::dna4>>);

/// Construct a fragmented owner at a stable address for one callback.
template <class R, class ScannerFactory, class Fn>
inline void with_fragmented_lazy_sequence(ScannerFactory&& scanner_fn, Fn&& fn) {
    using S = std::invoke_result_t<ScannerFactory>;
    lazy_sequence<R, S> lazy{std::forward<ScannerFactory>(scanner_fn)()};
    std::forward<Fn>(fn)(lazy);
}

/// Copy ranks through the bounded rank-window reader into contiguous output.
template <bool is_reversed, bool write_reversed = false,
          sequence_alphabet Alphabet>
[[gnu::always_inline]] inline void copy_sequence_ranks(
    lazy_sequence_view<Alphabet> sequence,
    std::span<std::uint8_t> destination,
    std::size_t from,
    std::size_t to)
{
    if (from > to || to > sequence.bp_size()
        || destination.size() < to - from) {
        abort_lazy_sequence_access("lazy sequence rank range is invalid");
    }
    if (from == to) {
        return;
    }
    const std::size_t length = to - from;
    auto reader = sequence.template rank_reader<
        is_reversed, write_reversed, std::uint8_t, 2>();
    const std::uint8_t* ranks = reader.read_at(
        static_cast<sequence_size_t>(from), length, destination);
    if (ranks != destination.data()) {
        std::memcpy(destination.data(), ranks, length);
    }
}

}  // namespace bioseq::storage
