/**
 * @file storage/lazy_sequence_view.cppm
 * @brief Representation-erased lazy-sequence view and allocation observer.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.storage.lazy_sequence_view;

import std;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.storage.scanner;
import bioseq.storage.size;
import bioseq.sequences;

export namespace bioseq::storage {

static_assert(__cplusplus >= 202302L);

namespace lazy_sequence_detail {

/** @brief Requires that one alphabet value supports seqan3 complement. */
template <class p_alphabet>
concept complementable_sequence_alphabet =
    requires(p_alphabet value) { seqan3::complement(std::move(value)); };

static_assert(complementable_sequence_alphabet<seq::dna4>);
static_assert(complementable_sequence_alphabet<seq::dna5>);
static_assert(!complementable_sequence_alphabet<seq::aa27>);

} // namespace lazy_sequence_detail

/** @brief Storage category reported by a traced sequence-payload allocation. */
enum class sequence_payload_allocation_kind : std::uint8_t {
    admitted_packed_sequence = 0u,
    flat_sequence,
    backing_copy,
};

/** @brief Immutable accounting record emitted after a payload allocation succeeds. */
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
 * task is submitted. The callback interprets them and is required to remain
 * alive until that task has joined/drained.  A null callback is the ordinary
 * non-traced route and costs only the allocation-path null check.
 */
struct sequence_payload_allocation_observer {
    /** @brief Publishes one allocation event with caller-owned context and correlations. */
    using publish_fn = void (*)(void*,
                                sequence_payload_allocation_event const&,
                                std::uint64_t,
                                std::uint64_t) noexcept;

    void* context{};
    publish_fn publish{};
    std::uint64_t correlation_a{};
    std::uint64_t correlation_b{};

    /** @brief Returns whether tracing callbacks are configured. */
    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && publish != nullptr;
    }
};

static_assert(std::is_trivially_copyable_v<sequence_payload_allocation_observer>);
static_assert(std::is_standard_layout_v<sequence_payload_allocation_observer>);

namespace allocation_observer_detail {

inline std::atomic<std::uint64_t> next_allocation_id{1u};

} // namespace allocation_observer_detail

/** @brief Publish one allocation event and return its nonzero trace identifier. */
[[nodiscard]] inline std::uint64_t
publish_sequence_allocation(sequence_payload_allocation_observer const* observer,
                            sequence_payload_allocation_kind kind,
                            std::uint64_t input_symbols,
                            std::uint64_t output_symbols,
                            std::uint64_t payload_bytes,
                            std::uint64_t retained_bytes) noexcept {
    if (observer == nullptr || !*observer) {
        return 0u;
    }
    std::uint64_t allocation_id =
        allocation_observer_detail::next_allocation_id.fetch_add(1u, std::memory_order_relaxed);
    if (allocation_id == 0u) {
        allocation_id =
            allocation_observer_detail::next_allocation_id.fetch_add(1u, std::memory_order_relaxed);
    }
    const sequence_payload_allocation_event event{
        .allocation_id = allocation_id,
        .input_symbols = input_symbols,
        .output_symbols = output_symbols,
        .payload_bytes = payload_bytes,
        .retained_bytes = retained_bytes,
        .kind = kind,
    };
    observer->publish(observer->context, event, observer->correlation_a, observer->correlation_b);
    return allocation_id;
}

/** @brief Exposes facade packing traits for one alphabet without a concrete owner. */
template <sequence_alphabet p_alphabet> struct lazy_sequence_traits {
    using alphabet_type = p_alphabet;
    using storage_unit = std::uint8_t;
    using packed_alphabet_range = compact_sequence_range<
        sequence_custom_alphabet<p_alphabet,
                                 std::uint8_t,
                                 seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>)>>;
    using packed_traits = typename packed_alphabet_range::traits;

    static constexpr std::size_t k_logical_bits_per_symbol =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);
    static constexpr std::size_t k_rank_stream_bytes_per_symbol = 1;
    static constexpr std::size_t k_alphabet_size = seqan3::alphabet_size<p_alphabet>;
    static_assert(k_logical_bits_per_symbol > 0 &&
                  k_logical_bits_per_symbol <= std::numeric_limits<unsigned char>::digits);

    /** @brief Returns the packed logical-rank complement of one storage unit. */
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
[[nodiscard]] constexpr std::size_t packed_letters_per_unit(std::uint8_t storage_unit_width,
                                                            std::uint8_t bits_per_symbol) noexcept {
    if (!(storage_unit_width == 1u || storage_unit_width == 2u || storage_unit_width == 4u ||
          storage_unit_width == 8u) ||
        bits_per_symbol == 0u || bits_per_symbol > std::numeric_limits<unsigned char>::digits) {
        return 0u;
    }
    return static_cast<std::size_t>(storage_unit_width) *
           std::numeric_limits<unsigned char>::digits / bits_per_symbol;
}

/** @brief Type-erased, non-owning logical-rank view over a lazy sequence owner. */
template <sequence_alphabet p_alphabet> class lazy_sequence_view {
  public:
    using alphabet_type = p_alphabet;
    using traits = lazy_sequence_traits<p_alphabet>;
    using slice_tag = lazy_sequence_slice_tag;

    /** @brief Reads one logical-rank window through an erased backend. */
    using read_at_fn = std::uint8_t const* (*)(void const*,
                                               sequence_size_t,
                                               std::size_t,
                                               std::span<std::uint8_t>);

    /** @brief Bit order used by resident packed descriptors. */
    enum class packed_symbol_order : std::uint8_t {
        least_significant_symbol_first,
    };

    /** @brief Borrowed packed region whose backing owner outlives this descriptor. */
    struct resident_packed_sequence {
        void const* units{};
        std::size_t unit_count{};
        sequence_size_t logical_length{};
        std::uint8_t storage_unit_width{};
        std::uint8_t bits_per_symbol{};
        std::uint16_t leading_symbol_offset{};
        std::uint16_t trailing_padding{};
        packed_symbol_order bit_order{packed_symbol_order::least_significant_symbol_first};
    };

    using resident_packed_result = std::optional<resident_packed_sequence>;
    /** @brief Probes whether a subrange can be borrowed as resident packed storage. */
    using resident_probe_fn = resident_packed_result (*)(void const*,
                                                         sequence_size_t,
                                                         sequence_size_t,
                                                         std::uint8_t) noexcept;
    /** @brief Copies one full packed sequence into an aligned destination buffer. */
    using copy_packed_sequence_fn = bool (*)(void const*, void*, std::size_t, std::uint8_t);
    /** @brief Copies one bounded interval of native packed storage units. */
    using copy_packed_window_fn = bool (*)(void const*,
                                           void*,
                                           std::size_t,
                                           std::size_t,
                                           std::uint8_t);

    /** @brief Complete callback table implementing one erased sequence backend. */
    struct backend_ops {
        /** @brief Returns the logical sequence length in base pairs. */
        sequence_size_t (*bp_size)(void const*) noexcept;
        /** @brief Orientation-specific logical-rank readers indexed by reversal flags. */
        std::array<read_at_fn, 4> read_at;
        /** @brief Attempts to borrow one resident packed subrange. */
        resident_probe_fn try_resident_packed;
        /** @brief Copies one packed sequence when the backend exposes a fast path. */
        copy_packed_sequence_fn copy_packed_sequence;
        /** @brief Copies native packed units covering one logical window. */
        copy_packed_window_fn copy_packed_window;
        /** @brief Reports whether the backend state may be shared across worker tasks. */
        bool (*worker_shareable)(void const*) noexcept;
    };

    /** @brief Binds one erased backend state table and optional allocation observer. */
    explicit lazy_sequence_view(
        void const* state,
        backend_ops const* ops,
        sequence_payload_allocation_observer const* observer = nullptr) noexcept
        : state_(state), ops_(ops), payload_observer_(observer) {
        DEBUG_ASSERT(
            ops_ != nullptr && ops_->bp_size != nullptr && ops_->worker_shareable != nullptr &&
                std::ranges::all_of(ops_->read_at,
                                    [](read_at_fn callback) { return callback != nullptr; }),
            "lazy sequence backend table must be complete");
    }

    /** @brief Returns the logical sequence length in base pairs. */
    [[nodiscard]] sequence_size_t bp_size() const noexcept {
        return ops_->bp_size(state_);
    }

    /** @brief Returns the logical sequence length in base pairs. */
    [[nodiscard]] sequence_size_t size() const noexcept {
        return bp_size();
    }

    /** @brief Returns whether the erased backend may enter worker tasks. */
    [[nodiscard]] bool worker_shareable() const noexcept {
        return ops_->worker_shareable(state_);
    }

    /** @brief Returns a copy of this view with a different allocation observer. */
    [[nodiscard]] lazy_sequence_view with_payload_allocation_observer(
        sequence_payload_allocation_observer const* observer) const noexcept {
        auto result = *this;
        result.payload_observer_ = observer;
        return result;
    }

    /** @brief Returns the allocation observer currently bound to this view. */
    [[nodiscard]] sequence_payload_allocation_observer const*
    payload_allocation_observer() const noexcept {
        return payload_observer_;
    }

    /** @brief Attempts to borrow one validated resident packed subrange. */
    [[nodiscard]] resident_packed_result try_resident_packed(
        sequence_size_t from, sequence_size_t to, std::uint8_t requested_width) const noexcept {
        if (from > to || to > bp_size() ||
            !(requested_width == 1u || requested_width == 2u || requested_width == 4u ||
              requested_width == 8u) ||
            ops_->try_resident_packed == nullptr) {
            return std::nullopt;
        }

        resident_packed_result result =
            ops_->try_resident_packed(state_, from, to, requested_width);
        if (!result.has_value()) {
            return std::nullopt;
        }

        const sequence_size_t length = to - from;
        constexpr std::uint8_t k_bits_per_symbol =
            static_cast<std::uint8_t>(traits::k_logical_bits_per_symbol);
        const sequence_size_t letters_per_unit = static_cast<sequence_size_t>(
            packed_letters_per_unit(requested_width, k_bits_per_symbol));
        const resident_packed_sequence& region = *result;

        if (letters_per_unit == 0u || region.logical_length != length ||
            region.storage_unit_width != requested_width ||
            region.bits_per_symbol != k_bits_per_symbol ||
            region.bit_order != packed_symbol_order::least_significant_symbol_first) {
            return std::nullopt;
        }
        if (length == 0u) {
            if (region.units != nullptr || region.unit_count != 0u ||
                region.leading_symbol_offset != 0u || region.trailing_padding != 0u) {
                return std::nullopt;
            }
            return result;
        }

        const auto occupied = bioseq::common::checked_add(
            length, static_cast<sequence_size_t>(region.leading_symbol_offset));
        if (region.units == nullptr ||
            reinterpret_cast<std::uintptr_t>(region.units) % requested_width != 0u ||
            region.unit_count > (std::numeric_limits<std::size_t>::max)() / requested_width ||
            region.leading_symbol_offset >= letters_per_unit || !occupied.has_value()) {
            return std::nullopt;
        }

        const sequence_size_t expected_units =
            bioseq::common::ceil_div(*occupied, letters_per_unit);
        const auto represented_symbols =
            bioseq::common::checked_mul(expected_units, letters_per_unit);
        if (expected_units >
                static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)()) ||
            region.unit_count != static_cast<std::size_t>(expected_units) ||
            !represented_symbols.has_value()) {
            return std::nullopt;
        }
        const sequence_size_t trailing = *represented_symbols - *occupied;
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
    [[nodiscard]] bool try_copy_packed_sequence(std::span<std::uint8_t> destination,
                                                std::size_t unit_count,
                                                std::uint8_t requested_width) const {
        if (ops_->copy_packed_sequence == nullptr ||
            !(requested_width == 1u || requested_width == 2u || requested_width == 4u ||
              requested_width == 8u) ||
            unit_count > (std::numeric_limits<std::size_t>::max)() / requested_width ||
            destination.size() != unit_count * requested_width ||
            (!destination.empty() &&
             reinterpret_cast<std::uintptr_t>(destination.data()) % requested_width != 0u)) {
            return false;
        }
        return ops_->copy_packed_sequence(state_, destination.data(), unit_count, requested_width);
    }

    /**
     * Copies only the native packed storage units intersecting `[from, to)`.
     * The callback never decodes, complements, or reorders biological symbols;
     * consumers receive the same LSB-first units described by
     * `resident_packed_sequence`, including the leading and trailing symbols in
     * the two boundary units.
     *
     * @return A validated descriptor over @p destination, or no value when the
     *         backend cannot provide native packed-window I/O.
     */
    [[nodiscard]] resident_packed_result
    try_copy_packed_window(std::span<std::uint8_t> destination,
                           sequence_size_t from,
                           sequence_size_t to,
                           std::uint8_t requested_width) const {
        if (from > to || to > bp_size() || ops_->copy_packed_window == nullptr ||
            !(requested_width == 1u || requested_width == 2u || requested_width == 4u ||
              requested_width == 8u) ||
            (!destination.empty() &&
             reinterpret_cast<std::uintptr_t>(destination.data()) % requested_width != 0u)) {
            return std::nullopt;
        }
        constexpr std::uint8_t k_bits_per_symbol =
            static_cast<std::uint8_t>(traits::k_logical_bits_per_symbol);
        const sequence_size_t letters_per_unit = static_cast<sequence_size_t>(
            packed_letters_per_unit(requested_width, k_bits_per_symbol));
        const sequence_size_t length = to - from;
        if (letters_per_unit == 0u) {
            return std::nullopt;
        }
        if (length == 0u) {
            return destination.empty()
                       ? resident_packed_result{resident_packed_sequence{
                             .units = nullptr,
                             .unit_count = 0u,
                             .logical_length = 0u,
                             .storage_unit_width = requested_width,
                             .bits_per_symbol = k_bits_per_symbol,
                             .leading_symbol_offset = 0u,
                             .trailing_padding = 0u,
                             .bit_order = packed_symbol_order::least_significant_symbol_first,
                         }}
                       : std::nullopt;
        }
        const sequence_size_t first_unit = from / letters_per_unit;
        const sequence_size_t leading = from % letters_per_unit;
        const auto occupied = bioseq::common::checked_add(leading, length);
        if (!occupied.has_value()) {
            return std::nullopt;
        }
        const sequence_size_t unit_count_u64 =
            bioseq::common::ceil_div(*occupied, letters_per_unit);
        const auto represented = bioseq::common::checked_mul(unit_count_u64, letters_per_unit);
        if (!represented.has_value() ||
            unit_count_u64 > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
            return std::nullopt;
        }
        const std::size_t unit_count = static_cast<std::size_t>(unit_count_u64);
        if (unit_count > (std::numeric_limits<std::size_t>::max)() / requested_width ||
            destination.size() != unit_count * requested_width ||
            first_unit > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
            return std::nullopt;
        }
        if (!ops_->copy_packed_window(state_,
                                      destination.data(),
                                      static_cast<std::size_t>(first_unit),
                                      unit_count,
                                      requested_width)) {
            return std::nullopt;
        }
        return resident_packed_sequence{
            .units = destination.data(),
            .unit_count = unit_count,
            .logical_length = length,
            .storage_unit_width = requested_width,
            .bits_per_symbol = k_bits_per_symbol,
            .leading_symbol_offset = static_cast<std::uint16_t>(leading),
            .trailing_padding = static_cast<std::uint16_t>(*represented - *occupied),
            .bit_order = packed_symbol_order::least_significant_symbol_first,
        };
    }

    /** @brief Copies logical ranks for one subrange into caller storage. */
    template <bool p_is_reversed, bool p_write_reversed = false>
    void copy_ranks(std::span<std::uint8_t> output, std::size_t from, std::size_t to) const {
        if (from > to || to > bp_size() || output.size() < to - from) {
            abort_lazy_sequence_access("lazy sequence rank range is invalid");
        }
        if (from == to) {
            return;
        }
        auto cursor = rank_reader<p_is_reversed, p_write_reversed, std::uint8_t, 2>();
        const std::size_t length = to - from;
        const std::uint8_t* source =
            cursor.read_at(static_cast<sequence_size_t>(from), length, output);
        if (source != output.data()) {
            std::memcpy(output.data(), source, length);
        }
    }

    /** @brief Two-slot logical-rank reader over one parent lazy sequence view. */
    template <bool p_is_reversed,
              bool p_write_reversed,
              typename p_stored_type,
              std::size_t p_max_loaded_slots = 2>
    struct rank_window_reader {
        lazy_sequence_view const* parent{};

        /** @brief Reads one logical-rank window through the parent backend. */
        [[nodiscard]] p_stored_type const* read_at(sequence_size_t bp_from,
                                                   std::size_t required_length,
                                                   std::span<p_stored_type> scratch) const {
            static_assert(std::same_as<p_stored_type, std::uint8_t>);
            static_assert(p_max_loaded_slots == 2, "facade backends implement the two-slot API");
            if (bp_from > parent->bp_size() || required_length > parent->bp_size() - bp_from ||
                scratch.size() < required_length) {
                abort_lazy_sequence_access("lazy sequence read_at range is invalid");
            }
            constexpr std::size_t k_orientation =
                (p_is_reversed ? 2u : 0u) | (p_write_reversed ? 1u : 0u);
            const std::uint8_t* result = parent->ops_->read_at[k_orientation](
                parent->state_, bp_from, required_length, scratch);
            if (required_length != 0u && result == nullptr) {
                abort_lazy_sequence_access("lazy sequence backend returned null");
            }
            return result;
        }
    };

    /** @brief Returns a rank window reader configured for the requested orientation. */
    template <bool p_is_reversed,
              bool p_write_reversed,
              typename p_stored_type,
              std::size_t p_max_loaded_slots = 2>
    [[nodiscard]] auto rank_reader() const noexcept {
        return rank_window_reader<p_is_reversed,
                                  p_write_reversed,
                                  p_stored_type,
                                  p_max_loaded_slots>{this};
    }

    /** @brief Chunked forward iterator over logical ranks in one lazy sequence view. */
    class rank_iterator {
        static constexpr std::size_t k_chunk_size = 256;

        lazy_sequence_view const* parent_{};
        sequence_size_t position_{};
        mutable sequence_size_t loaded_from_{};
        mutable std::size_t loaded_count_{};
        mutable std::array<std::uint8_t, k_chunk_size> scratch_{};
        mutable std::uint8_t const* loaded_{};

        /** @brief Loads the chunk containing the current iterator position. */
        void ensure_loaded() const {
            if (parent_ == nullptr || position_ >= parent_->bp_size()) {
                abort_lazy_sequence_access("lazy rank iterator dereferenced outside its sequence");
            }
            if (position_ >= loaded_from_ && position_ < loaded_from_ + loaded_count_) {
                return;
            }
            loaded_from_ = position_;
            loaded_count_ = static_cast<std::size_t>(
                std::min<sequence_size_t>(k_chunk_size, parent_->bp_size() - position_));
            loaded_ =
                parent_->ops_->read_at[0](parent_->state_, loaded_from_, loaded_count_, scratch_);
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

        /** @brief Constructs an unbound rank iterator. */
        rank_iterator() = default;
        /** @brief Constructs a rank iterator over one parent view and position. */
        rank_iterator(lazy_sequence_view const* parent, sequence_size_t position) noexcept
            : parent_(parent), position_(position) {}
        /** @brief Copy-constructs iterator state without sharing loaded chunks. */
        rank_iterator(rank_iterator const& other) noexcept
            : parent_(other.parent_), position_(other.position_) {}
        /** @brief Copy-assigns iterator state and drops any loaded chunk cache. */
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

        /** @brief Returns the logical rank at the current iterator position. */
        [[nodiscard]] std::uint8_t operator*() const {
            ensure_loaded();
            return loaded_[static_cast<std::size_t>(position_ - loaded_from_)];
        }
        /** @brief Advances the iterator by one logical rank. */
        rank_iterator& operator++() noexcept {
            if (parent_ == nullptr || position_ >= parent_->bp_size()) {
                abort_lazy_sequence_access("lazy rank iterator advanced past end");
            }
            ++position_;
            return *this;
        }
        /** @brief Advances the iterator by one logical rank and returns the previous value. */
        rank_iterator operator++(int) noexcept {
            rank_iterator previous = *this;
            ++*this;
            return previous;
        }
        /** @brief Advances the iterator by a signed logical-rank offset. */
        rank_iterator& operator+=(difference_type delta) {
            if (parent_ == nullptr) {
                abort_lazy_sequence_access("unbound lazy rank iterator cannot be advanced");
            }
            if (delta >= 0) {
                const auto magnitude = static_cast<sequence_size_t>(delta);
                if (position_ > parent_->bp_size() || magnitude > parent_->bp_size() - position_) {
                    abort_lazy_sequence_access("lazy rank iterator advanced past end");
                }
                position_ += magnitude;
            } else {
                const sequence_size_t magnitude =
                    sequence_size_t{0} - static_cast<sequence_size_t>(delta);
                if (magnitude > position_) {
                    abort_lazy_sequence_access("lazy rank iterator advanced before begin");
                }
                position_ -= magnitude;
            }
            loaded_count_ = 0u;
            loaded_ = nullptr;
            return *this;
        }
        /** @brief Compares two rank iterators for position equality. */
        [[nodiscard]] friend bool operator==(rank_iterator const& left,
                                             rank_iterator const& right) noexcept {
            return left.parent_ == right.parent_ && left.position_ == right.position_;
        }
    };

    /** @brief Returns a rank iterator at the first logical position. */
    [[nodiscard]] rank_iterator begin() const noexcept {
        return {this, 0u};
    }
    /** @brief Returns a rank iterator at the past-the-end logical position. */
    [[nodiscard]] rank_iterator end() const noexcept {
        return {this, bp_size()};
    }

    /** @brief Returns the iterator unchanged for logical-rank facade coordinates. */
    template <iterator_mode p_mode>
    [[nodiscard]] rank_iterator iter(rank_iterator value) const noexcept {
        static_assert(p_mode == iterator_mode::logical_rank,
                      "facade iter supports logical iterator_mode::logical_rank coordinates only");
        return value;
    }

    /** @brief Returns a logical-rank iterable subrange over this view. */
    template <iterator_mode p_mode> [[nodiscard]] auto iterable() const noexcept {
        static_assert(p_mode == iterator_mode::logical_rank,
                      "facade iteration is a logical-rank iterator_mode::logical_rank range");
        return std::ranges::subrange(begin(), end());
    }

  private:
    void const* state_{};
    backend_ops const* ops_{};
    sequence_payload_allocation_observer const* payload_observer_{};
};

/** @brief Return whether a view can safely enter a worker task. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline bool is_worker_shareable(lazy_sequence_view<p_alphabet> value) noexcept {
    return value.worker_shareable();
}

static_assert(lazy_sequence_slice<lazy_sequence_view<seq::dna4>>);
static_assert(resident_packed_sequence_view<lazy_sequence_view<seq::dna4>>);

/** @brief Owns one adopted backing retain together with its lazy view. */
template <sequence_alphabet p_alphabet> class retained_lazy_sequence_view {
  public:
    /** @brief Releases adopted backing retain state without throwing. */
    using release_fn = void (*)(void*) noexcept;

    /** @brief Adopts one backing retain together with its lazy sequence view. */
    retained_lazy_sequence_view(lazy_sequence_view<p_alphabet> view,
                                void* lifetime_state,
                                release_fn release) noexcept
        : view_(view), lifetime_state_(lifetime_state), release_(release) {
        DEBUG_ASSERT(lifetime_state_ != nullptr && release_ != nullptr,
                     "lazy view owner requires an adopted backing retain");
    }

    /** @brief Copying would duplicate unique backing retain ownership. */
    retained_lazy_sequence_view(retained_lazy_sequence_view const&) = delete;
    /** @brief Assignment would duplicate unique backing retain ownership. */
    retained_lazy_sequence_view& operator=(retained_lazy_sequence_view const&) = delete;
    /** @brief Move assignment is disabled to keep release semantics explicit. */
    retained_lazy_sequence_view& operator=(retained_lazy_sequence_view&&) = delete;

    /** @brief Move-constructs by transferring backing retain ownership. */
    retained_lazy_sequence_view(retained_lazy_sequence_view&& other) noexcept
        : view_(other.view_), lifetime_state_(other.lifetime_state_), release_(other.release_) {
        other.lifetime_state_ = nullptr;
        other.release_ = nullptr;
    }

    /** @brief Releases adopted backing retain state on destruction. */
    ~retained_lazy_sequence_view() noexcept {
        release_noexcept();
    }

    /** @brief Returns the non-owning lazy sequence view. */
    [[nodiscard]] lazy_sequence_view<p_alphabet> view() const noexcept {
        return view_;
    }

    /** @brief Releases adopted backing retain state and clears local handles. */
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
    lazy_sequence_view<p_alphabet> view_;
    void* lifetime_state_{};
    release_fn release_{};
};

} // namespace bioseq::storage
