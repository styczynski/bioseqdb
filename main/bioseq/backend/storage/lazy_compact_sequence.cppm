/*-------------------------------------------------------------------------
 *
 * storage/lazy_compact_sequence.cppm
 *
 * `lazy_compact_sequence<R, Scanner>` — the algorithm-side wrapper that
 * every consumer migrated in Phase B (DOC0263) takes instead of a
 * `compact_sequence_range<R>&`.
 *
 * Two `requires`-constrained partial specialisations expose IDENTICAL
 * signatures so algorithm code is template-monomorphic on the wrapper:
 *
 *   - NO-FRAGMENT  (Scanner::supports_fragments() == false): a thin pointer
 *     to the underlying compact_sequence_range. Zero overhead — pointer
 *     sized, every forwarder is [[gnu::always_inline]]. Production uses this
 *     (Scanner = sequence_scanner_none) so the migration is byte-for-byte
 *     identical to pre-Phase-B (DOC0263 R2).
 *   - FRAGMENTED  (Scanner::supports_fragments() == true): owns the Scanner
 *     and a fragment_cache, drives access one fragment at a time with
 *     aggressive eviction, exposes a fragment-aware forward iterator
 *     (current_fragment_no), `read_cptr(it, scratch, len)` (zero-copy fast
 *     path / caller-buffer stitch), an `encoded_iterator` with
 *     `ptr_at(...)`, and a uniform `to_encoded_into<is_reversed,
 *     write_reversed>(dst, from, to)` that the aligners call unchanged.
 *
 * DOC0263 §2.2 names this `bioseq_storage:lazy_compact_sequence`. As with the
 * scanner module, C++ module partitions can only be imported within the same
 * module, so this ships as a STANDALONE NO_POSTGRES module; the public types
 * still live in `bioseq::storage`.
 *
 * NO_POSTGRES note: the encoded-slot ring (DOC0263 §7) is specified to use a
 * palloc-backed MemoryContext. palloc does not exist in the NO_POSTGRES shared
 * library this module lives in, so the ring uses portable aligned malloc/free.
 * The load-bearing invariant (DOC0263 R12 / I37 / C3) — eviction frees the
 * actual bytes so memory is bounded by MaxLoadedSlots * fragment_bytes, NOT by
 * sequence length — is preserved.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/aligned_allocator.h>

#include "common.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <vector>

export module bioseq.storage.lazy;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.storage.scanner;
import bioseq.storage.coordinates;
import bioseq.storage.size;
import bioseq.common.fragment_cache;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.sequences.rank_pack;

static_assert(__cplusplus >= 202302L, "DOC0263 requires C++23-or-later");

export namespace bioseq::storage {

// =====================================================================
// DOC0377 §9 — one alphabet-specialized, representation-erased facade.
// =====================================================================

enum class sequence_payload_allocation_kind : std::uint8_t {
    admitted_packed_region = 0u,
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
        sequence_payload_allocation_kind::admitted_packed_region};
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

namespace lazy_payload_observer_detail {

inline std::atomic<std::uint64_t> next_allocation_id{1u};

} // namespace lazy_payload_observer_detail

[[nodiscard]] inline std::uint64_t observe_sequence_payload_allocation_noexcept(
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
        lazy_payload_observer_detail::next_allocation_id.fetch_add(
            1u, std::memory_order_relaxed);
    if (allocation_id == 0u) {
        allocation_id =
            lazy_payload_observer_detail::next_allocation_id.fetch_add(
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
struct lazy_rank_stream_traits {
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

template <sequence_alphabet Alphabet>
class basic_lazy_sequence_view {
public:
    using alphabet_type = Alphabet;
    using traits = lazy_rank_stream_traits<Alphabet>;
    using lazy_slice_tag = lazy_compact_sequence_slice_tag;

    using ptr_at_fn = std::uint8_t const* (*)(
        void const*, sequence_size_t, std::size_t, std::span<std::uint8_t>);

    enum class packed_bit_order : std::uint8_t {
        least_significant_symbol_first,
    };

    struct resident_packed_region {
        void const* units{};
        std::size_t unit_count{};
        sequence_size_t bp_length{};
        std::uint8_t storage_unit_byte_width{};
        std::uint8_t bits_per_symbol{};
        std::uint16_t leading_symbol_offset{};
        std::uint16_t trailing_padding_letters{};
        packed_bit_order bit_order{
            packed_bit_order::least_significant_symbol_first};
    };

    using resident_probe_result = std::optional<resident_packed_region>;
    using try_resident_packed_fn = resident_probe_result (*)(
        void const*, sequence_size_t, sequence_size_t, std::uint8_t) noexcept;
    using copy_full_packed_fn = bool (*)(
        void const*, void*, std::size_t, std::uint8_t);

    struct backend_ops {
        sequence_size_t (*bp_size)(void const*) noexcept;
        std::array<ptr_at_fn, 4> ptr_at;
        try_resident_packed_fn try_resident_packed;
        copy_full_packed_fn copy_full_packed;
        bool (*worker_shareable)(void const*) noexcept;
    };

    explicit basic_lazy_sequence_view(
            void const* state,
            backend_ops const* ops,
            sequence_payload_allocation_observer const* observer = nullptr)
            noexcept
        : state_(state), ops_(ops), payload_observer_(observer) {
        DEBUG_ASSERT(
            ops_ != nullptr && ops_->bp_size != nullptr
                && ops_->worker_shareable != nullptr
                && std::ranges::all_of(
                    ops_->ptr_at,
                    [](ptr_at_fn callback) { return callback != nullptr; }),
            "lazy sequence backend table must be complete");
    }

    [[nodiscard]] sequence_size_t bp_size() const noexcept {
        return ops_->bp_size(state_);
    }

    [[nodiscard]] sequence_size_t size() const noexcept { return bp_size(); }

    [[nodiscard]] bool worker_shareable() const noexcept {
        return ops_->worker_shareable(state_);
    }

    [[nodiscard]] basic_lazy_sequence_view with_payload_allocation_observer(
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

    [[nodiscard]] resident_probe_result try_resident_packed(
            sequence_size_t from,
            sequence_size_t to,
            std::uint8_t requested_width) const noexcept {
        if (from > to || to > bp_size()
            || !(requested_width == 1u || requested_width == 2u
                 || requested_width == 4u || requested_width == 8u)
            || ops_->try_resident_packed == nullptr) {
            return std::nullopt;
        }

        resident_probe_result result = ops_->try_resident_packed(
            state_, from, to, requested_width);
        if (!result.has_value()) {
            return std::nullopt;
        }

        const sequence_size_t length = to - from;
        constexpr std::uint8_t bits = static_cast<std::uint8_t>(
            traits::logical_bits_per_symbol);
        const sequence_size_t letters_per_unit =
            static_cast<sequence_size_t>(requested_width * CHAR_BIT / bits);
        const resident_packed_region& region = *result;

        if (letters_per_unit == 0u || region.bp_length != length
            || region.storage_unit_byte_width != requested_width
            || region.bits_per_symbol != bits
            || region.bit_order
                != packed_bit_order::least_significant_symbol_first) {
            return std::nullopt;
        }
        if (length == 0u) {
            if (region.units != nullptr || region.unit_count != 0u
                || region.leading_symbol_offset != 0u
                || region.trailing_padding_letters != 0u) {
                return std::nullopt;
            }
            return result;
        }

        if (region.units == nullptr
            || reinterpret_cast<std::uintptr_t>(region.units)
                    % requested_width
                != 0u
            || region.unit_count
                    > (std::numeric_limits<std::size_t>::max)()
                        / requested_width
            || region.leading_symbol_offset >= letters_per_unit
            || length
                    > (std::numeric_limits<sequence_size_t>::max)()
                        - region.leading_symbol_offset) {
            return std::nullopt;
        }

        const sequence_size_t occupied =
            length + region.leading_symbol_offset;
        const sequence_size_t expected_units =
            occupied / letters_per_unit
            + static_cast<sequence_size_t>(occupied % letters_per_unit != 0u);
        if (expected_units
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())
            || region.unit_count != static_cast<std::size_t>(expected_units)
            || expected_units
                    > (std::numeric_limits<sequence_size_t>::max)()
                        / letters_per_unit) {
            return std::nullopt;
        }
        const sequence_size_t trailing =
            expected_units * letters_per_unit - occupied;
        if (region.trailing_padding_letters != trailing) {
            return std::nullopt;
        }
        return result;
    }

    /**
     * Ask a backend with a native sequential packed-copy path to populate an
     * exact, aligned full-sequence destination. Unsupported representations
     * return false and leave generic rank streaming as the caller's fallback.
     */
    [[nodiscard]] bool try_copy_full_packed(
            std::span<std::uint8_t> destination,
            std::size_t unit_count,
            std::uint8_t requested_width) const {
        if (ops_->copy_full_packed == nullptr
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
        return ops_->copy_full_packed(
            state_, destination.data(), unit_count, requested_width);
    }

    template <bool IsReversed, bool WriteReversed = false>
    void to_encoded_into(std::span<std::uint8_t> output,
                         std::size_t from,
                         std::size_t to) const {
        if (from > to || to > bp_size() || output.size() < to - from) {
            lazy_fragment_panic("lazy sequence rank range is invalid");
        }
        if (from == to) {
            return;
        }
        auto cursor = encoded_iter<IsReversed, WriteReversed, std::uint8_t, 2>();
        const std::size_t length = to - from;
        const std::uint8_t* source = cursor.ptr_at(
            static_cast<sequence_size_t>(from), length, output);
        if (source != output.data()) {
            std::memcpy(output.data(), source, length);
        }
    }

    template <bool IsReversed,
              bool WriteReversed,
              typename StoredType,
              std::size_t MaxLoadedSlots = 2>
    struct encoded_iterator {
        basic_lazy_sequence_view const* parent{};

        [[nodiscard]] StoredType const* ptr_at(
                sequence_size_t bp_from,
                std::size_t required_length,
                std::span<StoredType> scratch) const {
            static_assert(std::same_as<StoredType, std::uint8_t>);
            static_assert(MaxLoadedSlots == 2,
                          "facade backends implement the two-slot API");
            if (bp_from > parent->bp_size()
                || required_length > parent->bp_size() - bp_from
                || scratch.size() < required_length) {
                lazy_fragment_panic("lazy sequence ptr_at range is invalid");
            }
            constexpr std::size_t orientation =
                (IsReversed ? 2u : 0u) | (WriteReversed ? 1u : 0u);
            const std::uint8_t* result = parent->ops_->ptr_at[orientation](
                parent->state_, bp_from, required_length, scratch);
            if (required_length != 0u && result == nullptr) {
                lazy_fragment_panic("lazy sequence backend returned null");
            }
            return result;
        }
    };

    template <bool IsReversed,
              bool WriteReversed,
              typename StoredType,
              std::size_t MaxLoadedSlots = 2>
    [[nodiscard]] auto encoded_iter() const noexcept {
        return encoded_iterator<IsReversed, WriteReversed,
                                StoredType, MaxLoadedSlots>{this};
    }

    class rank_iterator {
        static constexpr std::size_t chunk_size = 256;

        basic_lazy_sequence_view const* parent_{};
        sequence_size_t position_{};
        mutable sequence_size_t loaded_from_{};
        mutable std::size_t loaded_count_{};
        mutable std::array<std::uint8_t, chunk_size> scratch_{};
        mutable std::uint8_t const* loaded_{};

        void ensure_loaded() const {
            if (position_ >= loaded_from_
                && position_ < loaded_from_ + loaded_count_) {
                return;
            }
            loaded_from_ = position_;
            loaded_count_ = static_cast<std::size_t>(
                std::min<sequence_size_t>(
                    chunk_size, parent_->bp_size() - position_));
            loaded_ = parent_->ops_->ptr_at[0](
                parent_->state_, loaded_from_, loaded_count_, scratch_);
            if (loaded_count_ != 0u && loaded_ == nullptr) {
                lazy_fragment_panic("lazy rank backend returned null");
            }
        }

    public:
        using value_type = std::uint8_t;
        using difference_type = std::ptrdiff_t;
        using reference = std::uint8_t;
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;

        rank_iterator() = default;
        rank_iterator(basic_lazy_sequence_view const* parent,
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
            ++position_;
            return *this;
        }
        rank_iterator operator++(int) noexcept {
            rank_iterator previous = *this;
            ++*this;
            return previous;
        }
        rank_iterator& operator+=(difference_type delta) {
            if (delta >= 0) {
                const auto magnitude = static_cast<sequence_size_t>(delta);
                if (position_ > parent_->bp_size()
                    || magnitude > parent_->bp_size() - position_) {
                    lazy_fragment_panic(
                        "lazy rank iterator advanced past end");
                }
                position_ += magnitude;
            } else {
                const sequence_size_t magnitude = sequence_size_t{0}
                    - static_cast<sequence_size_t>(delta);
                if (magnitude > position_) {
                    lazy_fragment_panic(
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

    template <IteratorMode Mode>
    [[nodiscard]] rank_iterator iter(rank_iterator value) const noexcept {
        static_assert(Mode == BP,
                      "facade iter supports logical BP coordinates only");
        return value;
    }

    template <IteratorMode Mode>
    [[nodiscard]] auto iterable() const noexcept {
        static_assert(Mode == BP,
                      "facade iteration is a logical-rank BP range");
        return std::ranges::subrange(begin(), end());
    }

private:
    void const* state_{};
    backend_ops const* ops_{};
    sequence_payload_allocation_observer const* payload_observer_{};
};

template <sequence_alphabet Alphabet>
[[nodiscard]] inline bool lazy_is_worker_shareable(
        basic_lazy_sequence_view<Alphabet> value) noexcept {
    return value.worker_shareable();
}

static_assert(supports_lazy_compact_sequence_slice<
              basic_lazy_sequence_view<seq::dna4>>);
static_assert(supports_resident_packed_probe<
              basic_lazy_sequence_view<seq::dna4>>);

template <sequence_alphabet Alphabet>
class lazy_sequence_call_owner {
public:
    using release_fn = void (*)(void*) noexcept;

    lazy_sequence_call_owner(basic_lazy_sequence_view<Alphabet> view,
                             void* lifetime_state,
                             release_fn release) noexcept
        : view_(view), lifetime_state_(lifetime_state), release_(release) {
        DEBUG_ASSERT(lifetime_state_ != nullptr && release_ != nullptr,
                     "lazy view owner requires an adopted backing retain");
    }

    lazy_sequence_call_owner(lazy_sequence_call_owner const&) = delete;
    lazy_sequence_call_owner& operator=(lazy_sequence_call_owner const&) = delete;
    lazy_sequence_call_owner& operator=(lazy_sequence_call_owner&&) = delete;

    lazy_sequence_call_owner(lazy_sequence_call_owner&& other) noexcept
        : view_(other.view_),
          lifetime_state_(other.lifetime_state_),
          release_(other.release_) {
        other.lifetime_state_ = nullptr;
        other.release_ = nullptr;
    }

    ~lazy_sequence_call_owner() noexcept { release_noexcept(); }

    [[nodiscard]] basic_lazy_sequence_view<Alphabet> view() const noexcept {
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
    basic_lazy_sequence_view<Alphabet> view_;
    void* lifetime_state_{};
    release_fn release_{};
};

template <class R, class Scanner>
struct lazy_compact_sequence;

// =====================================================================
// Specialisation A — NO-FRAGMENT (Scanner::supports_fragments() == false)
//
// A thin reference wrapper around an existing compact_sequence_range<R>.
// Pointer-sized; every method [[gnu::always_inline]]-forwards to the
// underlying range. This is the production path (Scanner =
// sequence_scanner_none) and is byte-for-byte equivalent to passing the
// range directly (DOC0263 R2).
// =====================================================================

template <class R, class Scanner>
    requires (!Scanner::supports_fragments())
struct lazy_compact_sequence<R, Scanner> {
    using range_type   = R;
    using scanner_type = Scanner;
    using traits       = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;

    static constexpr std::size_t fragments_buffer_capacity     = 1;
    static constexpr std::size_t fragments_cleaning_max_loaded = 1;

    const compact_sequence_range<R>* _ref = nullptr;

    [[nodiscard]] [[gnu::always_inline]]
    explicit constexpr lazy_compact_sequence(const compact_sequence_range<R>& ref) noexcept
        : _ref(&ref) {}

    /// The underlying range. Algorithm code that has an all-in-RAM fast path
    /// (e.g. the SIMD minimizer kernel) calls this under
    /// `if constexpr (!Scanner::supports_fragments())`.
    [[nodiscard]] [[gnu::always_inline]]
    const compact_sequence_range<R>& underlying() const noexcept { return *_ref; }

    // ---- size / shape forwarders --------------------------------------
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t bp_size()   const noexcept { return static_cast<sequence_size_t>(_ref->bp_size()); }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t size()      const noexcept { return static_cast<sequence_size_t>(_ref->size()); }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t byte_size() const noexcept { return static_cast<sequence_size_t>(_ref->byte_size()); }
    [[nodiscard]] [[gnu::always_inline]]
    std::uint16_t   padding()   const noexcept { return static_cast<std::uint16_t>(_ref->padding()); }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t number_of_fragments() const noexcept { return 1; }

    // ---- iterator (the underlying range's iterator) -------------------
    using iterator_impl = typename compact_sequence_range<R>::iterator;
    [[nodiscard]] [[gnu::always_inline]] iterator_impl cbegin() const noexcept { return _ref->cbegin(); }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl cend()   const noexcept { return _ref->cend();   }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl begin()  const noexcept { return _ref->cbegin(); }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl end()    const noexcept { return _ref->cend();   }

    template <IteratorMode M, class I>
    [[nodiscard]] [[gnu::always_inline]] auto iter(const I& it) const noexcept
        { return _ref->template iter<M>(it); }
    template <IteratorMode M>
    [[nodiscard]] [[gnu::always_inline]] auto iterable() const noexcept
        { return _ref->template iterable<M>(); }

    template <class I>
    [[nodiscard]] [[gnu::always_inline]] std::string to_string(I b, I e, bool split_units = false) const
        { return _ref->to_string(b, e, split_units); }

    // ---- bit-cast view (exposed on the no-fragment side only) ---------
    template <class U>
    [[nodiscard]] [[gnu::always_inline]] auto bit_cast_to_storage_unit() const noexcept
        { return _ref->template bit_cast_to_storage_unit<U>(); }

    // ---- encode forwarders (the aligner-facing API) -------------------
    template <bool is_reversed, bool write_reversed = false>
    [[gnu::always_inline]]
    void to_encoded_into(std::span<std::uint8_t> dst, std::size_t bp_from, std::size_t bp_to) const {
        _ref->template to_encoded_into<is_reversed, write_reversed>(dst, bp_from, bp_to);
    }

    // ---- encoded_iterator (DOC0263 §7) — no-fragment specialisation ----
    // Exposes the SAME `encoded_iter<…>().ptr_at(bp_from, len, scratch)` surface
    // the migrated aligners call (§9.1/§9.2), so algorithm code is uniform over
    // both specialisations with no `if constexpr`. The bytes are fully resident,
    // so ptr_at just runs the existing all-in-RAM encoder into the caller's
    // scratch (no slot ring, no std::vector) and returns scratch.data().
    template <bool is_reversed, bool write_reversed, class stored_type_t, std::size_t MaxLoadedSlots = 2>
    struct encoded_iterator {
        const lazy_compact_sequence* _parent = nullptr;
        explicit encoded_iterator(const lazy_compact_sequence* parent) noexcept : _parent(parent) {}
        [[nodiscard]] const stored_type_t* ptr_at(sequence_size_t bp_from,
                                                  std::size_t required_length,
                                                  std::span<stored_type_t> scratch) const {
            if (required_length == 0) return scratch.data();
            if (scratch.size() < required_length) {
                lazy_fragment_panic("ptr_at: scratch too small for required_length");
            }
            static_assert(sizeof(stored_type_t) == 1,
                "no-fragment encoded_iter::ptr_at expects a 1-byte encoded type");
            _parent->_ref->template to_encoded_into<is_reversed, write_reversed>(
                std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(scratch.data()), required_length),
                static_cast<std::size_t>(bp_from),
                static_cast<std::size_t>(bp_from) + required_length);
            return scratch.data();
        }
    };
    template <bool is_reversed, bool write_reversed, class stored_type_t, std::size_t MaxLoadedSlots = 2>
    [[nodiscard]] auto encoded_iter() const noexcept {
        return encoded_iterator<is_reversed, write_reversed, stored_type_t, MaxLoadedSlots>(this);
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
// R2 (pointer-sized no-fragment wrapper) is verified by static_assert in
// lazy_compact_sequence_test.cpp against a concrete range type — a dummy R here
// would need a valid alphabet to form compact_sequence_range<R>::traits.

// =====================================================================
// Specialisation B — FRAGMENTED (Scanner::supports_fragments() == true)
// =====================================================================

template <class R, class Scanner>
    requires (Scanner::supports_fragments())
struct lazy_compact_sequence<R, Scanner> {
    using range_type   = R;
    using scanner_type = Scanner;
    using traits       = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;
    using handle_t     = sequence_fragment_handle<R>;

    static constexpr std::size_t fragment_storage_units        = Scanner::fragment_storage_units;
    static constexpr std::size_t fragments_buffer_capacity     = 16;
    static constexpr std::size_t fragments_cleaning_max_loaded = 1;
    static_assert(fragments_buffer_capacity > fragments_cleaning_max_loaded,
                  "buffer capacity must exceed the post-eviction target count");

    static constexpr std::size_t lpu           = traits::letters_per_unit;
    static constexpr std::size_t bits_per_char = traits::bits_per_char;
    static constexpr std::size_t frag_bp_count = fragment_storage_units * lpu;

    // DOC0263 R13 — non-copyable, non-movable (owns Scanner + cache; in-flight
    // iterators may hold slot references). Algorithm code takes it by reference;
    // it is constructed in place via with_lazy_fragmented(...).
    lazy_compact_sequence(const lazy_compact_sequence&)            = delete;
    lazy_compact_sequence& operator=(const lazy_compact_sequence&) = delete;
    lazy_compact_sequence(lazy_compact_sequence&&)                 = delete;
    lazy_compact_sequence& operator=(lazy_compact_sequence&&)      = delete;

    explicit lazy_compact_sequence(Scanner&& scanner) noexcept
        : _scanner(std::move(scanner)) {}
    ~lazy_compact_sequence() = default;

    // ---- size / shape -------------------------------------------------
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t bp_size()   const noexcept { return _scanner.total_bp_size(); }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t size()      const noexcept { return _scanner.total_size(); }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t byte_size() const noexcept { return _scanner.total_byte_size(); }
    [[nodiscard]] [[gnu::always_inline]]
    std::uint16_t   padding()   const noexcept {
        return static_cast<std::uint16_t>(_scanner.last_fragment_padding_letters());
    }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t number_of_fragments() const noexcept { return _scanner.number_of_fragments(); }
    [[nodiscard]] const Scanner& scanner() const noexcept { return _scanner; }

    // ---- unit-level iterator (DOC0263 §6.1) ---------------------------
    // Used by read_cptr and as cbegin()/cend(). Carries the global storage-unit
    // index; comparison includes the parent pointer (consolidated-review I28).
    struct iterator_impl {
        const lazy_compact_sequence* _parent      = nullptr;
        sequence_size_t              _global_unit = 0;

        [[gnu::always_inline]] iterator_impl& operator++() noexcept { ++_global_unit; return *this; }
        [[gnu::always_inline]] iterator_impl& operator+=(sequence_size_t n) noexcept { _global_unit += n; return *this; }

        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t current_fragment_no() const noexcept {
            return _global_unit / fragment_storage_units;
        }
        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t current_fragment_unit_offset() const noexcept {
            return _global_unit % fragment_storage_units;
        }
        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t global_unit_index() const noexcept { return _global_unit; }

        [[nodiscard]] [[gnu::always_inline]]
        friend bool operator==(const iterator_impl& a, const iterator_impl& b) noexcept {
            return a._parent == b._parent && a._global_unit == b._global_unit;
        }
        [[nodiscard]] [[gnu::always_inline]]
        friend std::strong_ordering operator<=>(const iterator_impl& a, const iterator_impl& b) noexcept {
            if (a._parent != b._parent) {
                lazy_fragment_panic("lazy iterator: cross-parent comparison");
            }
            return a._global_unit <=> b._global_unit;
        }
    };

    [[nodiscard]] iterator_impl cbegin() const noexcept { return iterator_impl{this, 0}; }
    [[nodiscard]] iterator_impl cend()   const noexcept { return iterator_impl{this, size()}; }
    [[nodiscard]] iterator_impl begin()  const noexcept { return cbegin(); }
    [[nodiscard]] iterator_impl end()    const noexcept { return cend(); }

    // ---- fragment management ------------------------------------------
    [[nodiscard]] const handle_t& _ensure_fragment(std::size_t n) const {
        auto key_of = [](const handle_t& h) noexcept { return h._fragment_no; };
        if (auto found = _fragments.find(n, key_of); found.has_value()) {
            return _fragments.slot_at(found->first);
        }
        _load_fragment_into(n);
        auto found2 = _fragments.find(n, key_of);
        if (!found2.has_value()) {
            lazy_fragment_panic("lazy: _ensure_fragment failed to load fragment");
        }
        return _fragments.slot_at(found2->first);
    }

    void _free_old_fragments() const {
        while (_fragments.valid_count() > fragments_cleaning_max_loaded) {
            _fragments.evict_oldest([this](handle_t& evicted) noexcept {
                _scanner.release_fragment(evicted._fragment_no);
            });
        }
    }

    // ---- read_cptr (DOC0263 §6.3) -------------------------------------
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it,
                                  std::span<storage_unit> scratch,
                                  std::size_t required_length) const {
        if (it._parent != this) {
            lazy_fragment_panic("read_cptr: iterator's parent doesn't match this wrapper");
        }
        const std::size_t cur_frag       = static_cast<std::size_t>(it._global_unit / fragment_storage_units);
        const std::size_t in_frag_offset = static_cast<std::size_t>(it._global_unit % fragment_storage_units);

        const handle_t& h = _ensure_fragment(cur_frag);
        // FAST PATH — request fits inside the current fragment. One compare.
        if (in_frag_offset + required_length <= h._data.size()) {
            return h._data.data() + in_frag_offset;
        }
        // SLOW PATH — stitch into the caller's scratch span.
        if (scratch.size() < required_length) {
            lazy_fragment_panic("read_cptr: scratch shorter than required_length");
        }
        std::size_t copied        = 0;
        std::size_t cur_unit      = in_frag_offset;
        std::size_t cur_frag_no   = cur_frag;
        while (copied < required_length) {
            const handle_t& cur_h  = _ensure_fragment(cur_frag_no);
            const std::size_t avail   = cur_h._data.size() - cur_unit;
            const std::size_t to_copy = std::min(avail, required_length - copied);
            std::memcpy(scratch.data() + copied, cur_h._data.data() + cur_unit,
                        to_copy * sizeof(storage_unit));
            copied += to_copy;
            cur_frag_no += 1;
            cur_unit = 0;
            if (cur_frag_no >= _scanner.number_of_fragments() && copied < required_length) {
                lazy_fragment_panic("read_cptr: required_length extends past end of sequence");
            }
        }
        _free_old_fragments();
        return scratch.data();
    }

    // Single-arg read_cptr — points into the current fragment (no stitch).
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it) const {
        const std::size_t cur_frag       = static_cast<std::size_t>(it._global_unit / fragment_storage_units);
        const std::size_t in_frag_offset = static_cast<std::size_t>(it._global_unit % fragment_storage_units);
        const handle_t& h = _ensure_fragment(cur_frag);
        return h._data.data() + in_frag_offset;
    }

    // ---- forward BP iterator (drives the scalar minimizer kernel) -----
    // Streams letters across fragments in global order; iterates exactly
    // bp_size() letters (matching compact_sequence_range's iter<BP> end, which
    // excludes trailing padding — sequences.cppm:1034). letter 0 sits at the
    // LSB of each storage unit (LSB-first, matching bp_scalar_iter).
    struct bp_iterator {
        const lazy_compact_sequence* _parent   = nullptr;
        sequence_size_t              _global_bp = 0;

        [[nodiscard]] storage_unit operator*() const {
            const std::size_t frag = static_cast<std::size_t>(_global_bp / frag_bp_count);
            const handle_t& h = _parent->_ensure_fragment(frag);
            const std::size_t bp_in_frag    = static_cast<std::size_t>(_global_bp % frag_bp_count);
            const std::size_t unit_in_frag  = bp_in_frag / lpu;
            const std::size_t letter_in_unit = bp_in_frag % lpu;
            const storage_unit u = h._data[unit_in_frag];
            return static_cast<storage_unit>(
                (u >> (letter_in_unit * bits_per_char)) & traits::char_mask);
        }
        [[gnu::always_inline]] bp_iterator& operator++() noexcept { ++_global_bp; return *this; }
        [[nodiscard]] [[gnu::always_inline]]
        friend bool operator==(const bp_iterator& a, const bp_iterator& b) noexcept {
            return a._parent == b._parent && a._global_bp == b._global_bp;
        }
    };

    [[nodiscard]] bp_iterator bp_begin() const noexcept { return bp_iterator{this, 0}; }
    [[nodiscard]] bp_iterator bp_end()   const noexcept { return bp_iterator{this, bp_size()}; }

    // iter<BP> / iterable<BP> — mirror compact_sequence_range_base's surface so
    // the scalar minimizer kernel can run over the fragmented wrapper unchanged.
    template <IteratorMode M, class I>
    [[nodiscard]] bp_iterator iter(const I& it) const noexcept {
        static_assert(M == BP,
                      "fragmented lazy iter<M>: only BP iteration is supported "
                      "(the minimizer kernel consumes ranks). SYMBOL is not "
                      "exercised on the fragmented path.");
        if (it._global_unit >= size()) {
            return bp_iterator{this, bp_size()};
        }
        return bp_iterator{this, it._global_unit * lpu};
    }

    struct bp_range {
        bp_iterator _b;
        bp_iterator _e;
        [[nodiscard]] bp_iterator begin() const noexcept { return _b; }
        [[nodiscard]] bp_iterator end()   const noexcept { return _e; }
    };
    template <IteratorMode M>
    [[nodiscard]] bp_range iterable() const noexcept {
        static_assert(M == BP, "fragmented lazy iterable<M>: only BP supported");
        return bp_range{bp_begin(), bp_end()};
    }

    // ---- (DOC0279 §17) the POC count_symbols() stub is deleted; the real
    //      per-rank tally is tally_symbol_counts() in bioseq_indexing.symbol_counts.

    // ---- to_encoded_into (the aligner-facing encode) ------------------
    // DOC0263 §1.1#7 / §7: the fragmented encode goes through the
    // `encoded_iterator` slot ring (NO `std::vector<uint8_t>` anywhere on the
    // encoded path). This is a thin convenience over `encoded_iter().ptr_at()`
    // — the same mechanism the migrated aligners call directly (§9.1/§9.2) —
    // stitching into the caller's `dst` (bounded by the region length, R3).
    template <bool is_reversed, bool write_reversed = false>
    void to_encoded_into(std::span<std::uint8_t> dst, std::size_t bp_from, std::size_t bp_to) const {
        if (bp_from >= bp_to) return;
        const std::size_t region_length = bp_to - bp_from;
        auto eit = encoded_iter<is_reversed, write_reversed, std::uint8_t, 2>();
        const std::uint8_t* p = eit.ptr_at(
            static_cast<sequence_size_t>(bp_from), region_length, dst);
        if (p != dst.data()) {
            std::memcpy(dst.data(), p, region_length * sizeof(std::uint8_t));
        }
    }

    // ---- bit_cast trap (DOC0263 §8.5) ---------------------------------
    template <class U>
    auto bit_cast_to_storage_unit() const noexcept {
        static_assert(sizeof(U) == 0,
            "lazy_compact_sequence::bit_cast_to_storage_unit is unavailable on the "
            "fragmented specialisation (lanes spread across fragments). Gate the "
            "bit-cast path via `if constexpr (Scanner::supports_fragments())` and "
            "take the non-bit-cast path (DOC0263 §8.5).");
    }

    // ---- encoded_iterator (DOC0263 §7) --------------------------------
    // Per-fragment RAW-forward-rank slot ring (portable malloc, bounded by
    // MaxLoadedSlots fragments — independent of sequence length per R12/§13.1;
    // NO std::vector anywhere on the encoded path, §1.1#7). `ptr_at` applies
    // the exact emit_letter orientation transform (sequences.cppm:1386) — the
    // reverse-strand coordinate mirror, the per-letter complement iff
    // is_reversed, and the descending write order iff exactly one of
    // (is_reversed, write_reversed) is set — so its output is byte-identical to
    // the all-in-RAM encoder for every combination. The §5 zero-copy fast path
    // returns a pointer straight into a resident slot for the plain-forward
    // encode that fits one fragment; every other case stitches into the
    // caller's scratch with a forward-sequential per-fragment gather (each
    // fragment is encoded at most once, so there is no re-encode thrash even
    // for multi-fragment regions). Eviction frees the slot bytes (R12).
    template <bool is_reversed, bool write_reversed, class stored_type_t, std::size_t MaxLoadedSlots = 2>
    struct encoded_iterator {
        struct encoded_slot {
            stored_type_t* bytes       = nullptr;          // malloc'd: raw forward ranks
            std::size_t    fragment_no = std::numeric_limits<std::size_t>::max();
            std::size_t    bp_count    = 0;
        };

        const lazy_compact_sequence* _parent = nullptr;
        mutable bioseq::common::fragment_cache<encoded_slot, MaxLoadedSlots + 1> _slots{};

        explicit encoded_iterator(const lazy_compact_sequence* parent) noexcept : _parent(parent) {}
        encoded_iterator(const encoded_iterator&)            = delete;
        encoded_iterator& operator=(const encoded_iterator&) = delete;
        encoded_iterator(encoded_iterator&&)                 = delete;
        encoded_iterator& operator=(encoded_iterator&&)      = delete;
        ~encoded_iterator() {
            // Free every resident slot's bytes.
            for (std::size_t i = 0; i < _slots.capacity; ++i) {
                _slots.evict_at(i, [](encoded_slot& s) noexcept { std::free(s.bytes); s.bytes = nullptr; });
            }
        }

        // Encode fragment `frag` as RAW FORWARD ranks (no complement, no
        // reorder) so a resident slot is reusable across (is_reversed,
        // write_reversed); the orientation transform is applied by ptr_at.
        encoded_slot& _ensure_encoded_slot(std::size_t frag) const {
            auto key_of = [](const encoded_slot& s) noexcept { return s.fragment_no; };
            if (auto found = _slots.find(frag, key_of); found.has_value()) {
                return _slots.slot_at(found->first);
            }
            const handle_t& h = _parent->_ensure_fragment(frag);
            const std::size_t bp_count = static_cast<std::size_t>(h._bp_letters);
            stored_type_t* bytes =
                static_cast<stored_type_t*>(std::malloc(_parent->frag_bp_count * sizeof(stored_type_t)));
            if (bytes == nullptr) { lazy_fragment_panic("encoded_iterator: slot malloc failed"); }
            for (std::size_t bp = 0; bp < bp_count; ++bp) {
                const std::size_t unit_in_frag  = bp / lpu;
                const std::size_t letter_in_unit = bp % lpu;
                const storage_unit u = h._data[unit_in_frag];
                const storage_unit rank = static_cast<storage_unit>(
                    (u >> (letter_in_unit * bits_per_char)) & traits::char_mask);
                bytes[bp] = static_cast<stored_type_t>(rank);
            }
            auto [idx, gen] = _slots.push(encoded_slot{bytes, frag, bp_count},
                [](encoded_slot& evicted) noexcept { std::free(evicted.bytes); evicted.bytes = nullptr; });
            (void)gen;
            // Aggressive eviction: keep at most MaxLoadedSlots resident.
            while (_slots.valid_count() > MaxLoadedSlots) {
                _slots.evict_oldest([](encoded_slot& evicted) noexcept {
                    std::free(evicted.bytes); evicted.bytes = nullptr;
                });
            }
            return _slots.slot_at(idx);
        }

        // ptr_at(bp_from, len, scratch): `bp_from` is in the SAME coordinate
        // convention as compact_sequence_range::to_encoded_into — region
        // coordinates of the (possibly reversed) OUTPUT sequence. Returns a
        // pointer to `len` encoded bytes: a resident slot (zero-copy) for the
        // plain-forward fast path, else `scratch.data()`.
        [[nodiscard]]
        const stored_type_t* ptr_at(sequence_size_t bp_from, std::size_t required_length,
                                    std::span<stored_type_t> scratch) const {
            if (required_length == 0) return scratch.data();
            const std::size_t total = static_cast<std::size_t>(_parent->bp_size());
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
                const std::size_t fragment_bp = static_cast<std::size_t>(_parent->frag_bp_count);
                const std::size_t cur_frag    = fwd_from / fragment_bp;
                const std::size_t in_frag_bp  = fwd_from % fragment_bp;
                const encoded_slot& slot = _ensure_encoded_slot(cur_frag);
                if (in_frag_bp + required_length <= slot.bp_count) {
                    return slot.bytes + in_frag_bp;
                }
            }
            if (scratch.size() < required_length) {
                lazy_fragment_panic("ptr_at: scratch too small for required_length");
            }
            // Forward-sequential per-fragment gather of raw ranks for
            // [fwd_from, fwd_from+len); apply emit_letter (complement iff
            // is_reversed; descending output iff descending_write).
            const std::size_t fragment_bp = static_cast<std::size_t>(_parent->frag_bp_count);
            std::size_t gathered = 0;
            std::size_t src_bp   = fwd_from;
            while (gathered < required_length) {
                const std::size_t frag    = src_bp / fragment_bp;
                const std::size_t in_frag = src_bp % fragment_bp;
                const encoded_slot& s = _ensure_encoded_slot(frag);
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
                    lazy_fragment_panic("ptr_at: required_length extends past end of sequence");
                }
            }
            return scratch.data();
        }

        [[nodiscard]] stored_type_t operator[](sequence_size_t bp_idx) const {
            stored_type_t one{};
            std::span<stored_type_t> sc(&one, 1);
            const stored_type_t* p = ptr_at(bp_idx, 1, sc);
            return p[0];
        }
    };

    template <bool is_reversed, bool write_reversed, class stored_type_t, std::size_t MaxLoadedSlots = 2>
    [[nodiscard]] auto encoded_iter() const noexcept {
        return encoded_iterator<is_reversed, write_reversed, stored_type_t, MaxLoadedSlots>(this);
    }

private:
    mutable Scanner _scanner;
    mutable bioseq::common::fragment_cache<handle_t, fragments_buffer_capacity> _fragments{};

    void _load_fragment_into(std::size_t n) const {
        handle_t handle;
        handle._data            = _scanner.read_fragment(n);
        handle._fragment_no     = n;
        handle._global_bp_offset = static_cast<sequence_size_t>(n) * frag_bp_count;
        const bool is_last = (n + 1 == _scanner.number_of_fragments());
        handle._bp_letters = is_last
            ? (_scanner.total_bp_size() - handle._global_bp_offset)
            : static_cast<sequence_size_t>(frag_bp_count);
        handle._padding = is_last
            ? static_cast<std::uint16_t>(_scanner.last_fragment_padding_letters())
            : std::uint16_t{0};
        _fragments.push(handle, [this](handle_t& evicted) noexcept {
            _scanner.release_fragment(evicted._fragment_no);
        });
        _free_old_fragments();
    }
};

// =====================================================================
// Factory + callback dispatchers (DOC0263 §3.3, §4.2)
// =====================================================================

/// Wrap an in-memory range in the zero-overhead no-fragment lazy wrapper.
template <class R>
[[nodiscard]] [[gnu::always_inline]]
inline lazy_compact_sequence<R, sequence_scanner_none<
        typename compact_sequence_range<R>::traits::storage_unit>>
make_lazy_in_memory(const compact_sequence_range<R>& range) noexcept {
    return lazy_compact_sequence<R, sequence_scanner_none<
        typename compact_sequence_range<R>::traits::storage_unit>>{range};
}

/// Construct an in-memory lazy wrapper around `range` and invoke `fn(lazy)`.
/// Zero-cost: the wrapper is a thin reference.
template <class R, class Fn>
[[gnu::always_inline]]
inline void with_lazy_in_memory(const compact_sequence_range<R>& range, Fn&& fn) {
    lazy_compact_sequence<R, sequence_scanner_none<
        typename compact_sequence_range<R>::traits::storage_unit>> lazy{range};
    std::forward<Fn>(fn)(lazy);
}

template <class R, class Scanner>
[[nodiscard]] inline bool lazy_bytes_are_resident(
        const lazy_compact_sequence<R, Scanner>& sequence) noexcept {
    if constexpr (Scanner::supports_fragments()) {
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
[[nodiscard]] inline bool lazy_is_worker_shareable(
        const lazy_compact_sequence<R, Scanner>&) noexcept {
    return !Scanner::supports_fragments();
}

namespace lazy_view_detail {

template <class Alphabet>
concept complementable_sequence_alphabet = requires (Alphabet value) {
    seqan3::complement(std::move(value));
};

static_assert(complementable_sequence_alphabet<seq::dna4>);
static_assert(complementable_sequence_alphabet<seq::dna5>);
static_assert(!complementable_sequence_alphabet<seq::aa27>);

template <bool IsReversed, bool WriteReversed, class R, class Scanner>
[[nodiscard]] const std::uint8_t* existing_lazy_ptr_at(
        void const* state,
        sequence_size_t bp_from,
        std::size_t required_length,
        std::span<std::uint8_t> scratch) {
    const auto& sequence =
        *static_cast<const lazy_compact_sequence<R, Scanner>*>(state);
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
        auto cursor = sequence.template encoded_iter<
            IsReversed, WriteReversed, std::uint8_t, 2>();
        const std::uint8_t* result = cursor.ptr_at(
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
[[nodiscard]] sequence_size_t existing_lazy_bp_size(
        void const* state) noexcept {
    return static_cast<const lazy_compact_sequence<R, Scanner>*>(state)
        ->bp_size();
}

template <class R, class Scanner>
[[nodiscard]] bool existing_lazy_worker_shareable(
        void const* state) noexcept {
    const auto& sequence =
        *static_cast<const lazy_compact_sequence<R, Scanner>*>(state);
    return lazy_is_worker_shareable(sequence);
}

template <class R, class Scanner>
[[nodiscard]] auto existing_lazy_resident_probe(
        void const* state,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width) noexcept
    -> typename basic_lazy_sequence_view<
        typename compact_sequence_range<R>::traits::alphabet_type>::
        resident_probe_result {
    using range_traits = typename compact_sequence_range<R>::traits;
    using alphabet_type = typename range_traits::alphabet_type;
    using view_type = basic_lazy_sequence_view<alphabet_type>;
    using descriptor_type = typename view_type::resident_packed_region;

    if constexpr (Scanner::supports_fragments()) {
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
            lazy_rank_stream_traits<alphabet_type>::logical_bits_per_symbol);
        if (requested_width != physical_width
            || range_traits::bits_per_char != logical_bits) {
            return std::nullopt;
        }

        const sequence_size_t length = to - from;
        if (length == 0u) {
            return descriptor_type{
                .units = nullptr,
                .unit_count = 0u,
                .bp_length = 0u,
                .storage_unit_byte_width = physical_width,
                .bits_per_symbol = logical_bits,
                .leading_symbol_offset = 0u,
                .trailing_padding_letters = 0u,
                .bit_order = view_type::packed_bit_order::
                    least_significant_symbol_first,
            };
        }

        constexpr sequence_size_t letters_per_unit =
            range_traits::letters_per_unit;
        const sequence_size_t first_unit = from / letters_per_unit;
        const sequence_size_t leading = from % letters_per_unit;
        const sequence_size_t occupied = leading + length;
        const sequence_size_t unit_count =
            occupied / letters_per_unit
            + static_cast<sequence_size_t>(occupied % letters_per_unit != 0u);
        const sequence_size_t trailing =
            unit_count * letters_per_unit - occupied;

        const auto& sequence =
            *static_cast<const lazy_compact_sequence<R, Scanner>*>(state);
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
            .bp_length = length,
            .storage_unit_byte_width = physical_width,
            .bits_per_symbol = logical_bits,
            .leading_symbol_offset = static_cast<std::uint16_t>(leading),
            .trailing_padding_letters = static_cast<std::uint16_t>(trailing),
            .bit_order = view_type::packed_bit_order::
                least_significant_symbol_first,
        };
    }
}

template <class R, class Scanner>
[[nodiscard]] bool existing_lazy_copy_full_packed(
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
            lazy_rank_stream_traits<alphabet_type>::logical_bits_per_symbol;
        constexpr std::size_t letters_per_unit = range_traits::letters_per_unit;
        const auto& sequence =
            *static_cast<const lazy_compact_sequence<R, Scanner>*>(state);
        const sequence_size_t expected_units =
            sequence.bp_size() / letters_per_unit
            + static_cast<sequence_size_t>(
                sequence.bp_size() % letters_per_unit != 0u);
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
struct existing_lazy_adapter {
    using alphabet_type =
        typename compact_sequence_range<R>::traits::alphabet_type;
    using view_type = basic_lazy_sequence_view<alphabet_type>;

    static inline const typename view_type::backend_ops ops{
        .bp_size = &existing_lazy_bp_size<R, Scanner>,
        .ptr_at = {
            &existing_lazy_ptr_at<false, false, R, Scanner>,
            &existing_lazy_ptr_at<false, true, R, Scanner>,
            &existing_lazy_ptr_at<true, false, R, Scanner>,
            &existing_lazy_ptr_at<true, true, R, Scanner>,
        },
        .try_resident_packed = &existing_lazy_resident_probe<R, Scanner>,
        .copy_full_packed = &existing_lazy_copy_full_packed<R, Scanner>,
        .worker_shareable = &existing_lazy_worker_shareable<R, Scanner>,
    };
};

// Inline compact ranges are already stable owners. Adapt them directly so
// callers do not need to name or retain the concrete no-fragment scanner
// wrapper merely to cross the canonical facade boundary. Each data callback
// creates the pointer-sized wrapper locally; encoded bytes are copied into the
// caller scratch before it returns, while a resident probe points into the
// range's own storage.
template <bool IsReversed, bool WriteReversed, class R>
[[nodiscard]] const std::uint8_t* compact_range_ptr_at(
        void const* state,
        sequence_size_t bp_from,
        std::size_t required_length,
        std::span<std::uint8_t> scratch) {
    using scanner_type = sequence_scanner_none<
        typename compact_sequence_range<R>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<R>*>(state);
    const lazy_compact_sequence<R, scanner_type> owner{range};
    return existing_lazy_ptr_at<IsReversed, WriteReversed, R, scanner_type>(
        std::addressof(owner), bp_from, required_length, scratch);
}

template <class R>
[[nodiscard]] sequence_size_t compact_range_bp_size(
        void const* state) noexcept {
    using scanner_type = sequence_scanner_none<
        typename compact_sequence_range<R>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<R>*>(state);
    const lazy_compact_sequence<R, scanner_type> owner{range};
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
    -> typename basic_lazy_sequence_view<
        typename compact_sequence_range<R>::traits::alphabet_type>::
        resident_probe_result {
    using scanner_type = sequence_scanner_none<
        typename compact_sequence_range<R>::traits::storage_unit>;
    const auto& range = *static_cast<const compact_sequence_range<R>*>(state);
    const lazy_compact_sequence<R, scanner_type> owner{range};
    return existing_lazy_resident_probe<R, scanner_type>(
        std::addressof(owner), from, to, requested_width);
}

template <class R>
struct compact_range_adapter {
    using alphabet_type =
        typename compact_sequence_range<R>::traits::alphabet_type;
    using view_type = basic_lazy_sequence_view<alphabet_type>;

    static inline const typename view_type::backend_ops ops{
        .bp_size = &compact_range_bp_size<R>,
        .ptr_at = {
            &compact_range_ptr_at<false, false, R>,
            &compact_range_ptr_at<false, true, R>,
            &compact_range_ptr_at<true, false, R>,
            &compact_range_ptr_at<true, true, R>,
        },
        .try_resident_packed = &compact_range_resident_probe<R>,
        .copy_full_packed = nullptr,
        .worker_shareable = &compact_range_worker_shareable<R>,
    };
};

}  // namespace lazy_view_detail

template <class R, class Scanner>
[[nodiscard]] inline auto as_lazy_sequence_view(
        const lazy_compact_sequence<R, Scanner>& owner) noexcept {
    using adapter = lazy_view_detail::existing_lazy_adapter<R, Scanner>;
    return typename adapter::view_type{
        std::addressof(owner), std::addressof(adapter::ops)};
}

template <class R>
[[nodiscard]] inline auto as_lazy_sequence_view(
        const compact_sequence_range<R>& owner) noexcept {
    using adapter = lazy_view_detail::compact_range_adapter<R>;
    return typename adapter::view_type{
        std::addressof(owner), std::addressof(adapter::ops)};
}

/**
 * Invoke a callback through the synthetic fragmented scanner used by the
 * integration parity GUC.  The concrete scanner and lazy owner remain inside
 * the storage module; callers receive only the canonical facade by value and
 * cannot accidentally flatten the route while migrating consumer types.
 */
template <std::size_t FragmentStorageUnits =
              SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT,
          class R, class Fn>
inline void with_lazy_test_chunked(
        const compact_sequence_range<R>& range,
        Fn&& fn) {
    using scanner_type = sequence_scanner_test_chunked<
        typename compact_sequence_range<R>::traits::storage_unit,
        FragmentStorageUnits>;
    lazy_compact_sequence<R, scanner_type> owner{
        scanner_type::from_range(range)};
    std::forward<Fn>(fn)(as_lazy_sequence_view(owner));
}

/** Keep two synthetic fragmented owners alive for one facade-only callback. */
template <std::size_t FragmentStorageUnits =
              SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT,
          class LeftR, class RightR, class Fn>
inline void with_lazy_test_chunked(
        const compact_sequence_range<LeftR>& left_range,
        const compact_sequence_range<RightR>& right_range,
        Fn&& fn) {
    using left_alphabet =
        typename compact_sequence_range<LeftR>::traits::alphabet_type;
    using right_alphabet =
        typename compact_sequence_range<RightR>::traits::alphabet_type;
    static_assert(std::is_same_v<left_alphabet, right_alphabet>,
                  "chunked facade pair must share one alphabet");
    with_lazy_test_chunked<FragmentStorageUnits>(
        left_range,
        [&right_range, &fn](auto left_view) {
            with_lazy_test_chunked<FragmentStorageUnits>(
                right_range,
                [left_view, &fn](auto right_view) {
                    std::forward<Fn>(fn)(left_view, right_view);
                });
        });
}

template <sequence_transform Transform,
          sequence_alphabet Alphabet,
          class OutputWriter>
void stream_transformed_packed(
        basic_lazy_sequence_view<Alphabet> view,
        std::uint64_t from,
        std::uint64_t to,
        OutputWriter& output) {
    if (from > to || to > view.bp_size()) {
        throw std::invalid_argument(
            "transformed sequence stream range is invalid");
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

    if constexpr (has_reverse(Transform)) {
        std::uint64_t position = to;
        while (position > from) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(scratch.size(), position - from));
            const std::uint64_t chunk_from = position - count;
            view.template to_encoded_into<false, true>(
                std::span<std::uint8_t>{scratch.data(), count},
                static_cast<std::size_t>(chunk_from),
                static_cast<std::size_t>(position));
            if constexpr (has_complement(Transform)) {
                static_assert(
                    lazy_view_detail::complementable_sequence_alphabet<
                        Alphabet>);
                for (std::size_t index = 0u; index < count; ++index) {
                    scratch[index] = lazy_rank_stream_traits<Alphabet>::
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
            view.template to_encoded_into<false, false>(
                std::span<std::uint8_t>{scratch.data(), count},
                static_cast<std::size_t>(position),
                static_cast<std::size_t>(position + count));
            if constexpr (has_complement(Transform)) {
                static_assert(
                    lazy_view_detail::complementable_sequence_alphabet<
                        Alphabet>);
                for (std::size_t index = 0u; index < count; ++index) {
                    scratch[index] = lazy_rank_stream_traits<Alphabet>::
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

// One chunk-independent lexicographic core. Chunk boundaries carry no semantic
// weight; views::join naturally continues across unequal chunk sizes.
template <class LeftChunks, class RightChunks>
[[nodiscard]] int compare_logical_rank_streams(
        LeftChunks&& left,
        RightChunks&& right) {
    auto left_ranks = std::views::join(
        std::views::all(std::forward<LeftChunks>(left)));
    auto right_ranks = std::views::join(
        std::views::all(std::forward<RightChunks>(right)));
    auto left_it = left_ranks.begin();
    auto right_it = right_ranks.begin();
    const auto left_end = left_ranks.end();
    const auto right_end = right_ranks.end();
    while (left_it != left_end && right_it != right_end) {
        const auto left_rank = static_cast<std::uint8_t>(*left_it);
        const auto right_rank = static_cast<std::uint8_t>(*right_it);
        if (left_rank < right_rank) {
            return -1;
        }
        if (left_rank > right_rank) {
            return 1;
        }
        ++left_it;
        ++right_it;
    }
    if (left_it == left_end && right_it == right_end) {
        return 0;
    }
    return left_it == left_end ? -1 : 1;
}

template <sequence_alphabet Alphabet>
[[nodiscard]] int compare_sequence_views(
        basic_lazy_sequence_view<Alphabet> left,
        basic_lazy_sequence_view<Alphabet> right) {
    struct comparison_writer {
        typename basic_lazy_sequence_view<Alphabet>::rank_iterator cursor;
        typename basic_lazy_sequence_view<Alphabet>::rank_iterator end;
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
    stream_transformed_packed<sequence_transform::identity>(
        left, 0u, left.bp_size(), writer);
    if (writer.result != 0) {
        return writer.result;
    }
    return writer.cursor == writer.end ? 0 : -1;
}

namespace lazy_view_detail {

struct logical_hash_writer_v1 {
    static constexpr std::uint64_t fnv_offset_basis =
        14695981039346656037ull;
    static constexpr std::uint64_t fnv_prime = 1099511628211ull;

    explicit logical_hash_writer_v1(std::uint64_t seed) noexcept
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

}  // namespace lazy_view_detail

template <std::ranges::input_range RankRange>
[[nodiscard]] std::uint64_t sequence_logical_hash_bytes_v1(
        RankRange&& ranks,
        std::uint64_t logical_length,
        std::uint64_t seed) {
    lazy_view_detail::logical_hash_writer_v1 writer{seed};
    for (auto&& rank : ranks) {
        const std::uint8_t value = static_cast<std::uint8_t>(rank);
        writer.append_ranks(std::span<const std::uint8_t>{&value, 1u});
    }
    return writer.finish(logical_length);
}

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint64_t sequence_logical_hash_v1(
        basic_lazy_sequence_view<Alphabet> view,
        std::uint64_t seed) {
    lazy_view_detail::logical_hash_writer_v1 writer{seed};
    stream_transformed_packed<sequence_transform::identity>(
        view, 0u, view.bp_size(), writer);
    return writer.finish(view.bp_size());
}

namespace lazy_view_detail {

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
        typename basic_lazy_sequence_view<Alphabet>::resident_packed_region;
    descriptor_type const* descriptor{};
};

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint64_t load_resident_unit(
        resident_view_ops_context<Alphabet> const& context,
        std::size_t unit_index) noexcept {
    const auto& descriptor = *context.descriptor;
    const auto* bytes = static_cast<const std::uint8_t*>(descriptor.units)
        + unit_index * descriptor.storage_unit_byte_width;
    switch (descriptor.storage_unit_byte_width) {
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
        descriptor.storage_unit_byte_width * CHAR_BIT
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
[[nodiscard]] const std::uint8_t* resident_ptr_at(
        void const* state,
        sequence_size_t bp_from,
        std::size_t required_length,
        std::span<std::uint8_t> scratch) {
    const auto& context =
        *static_cast<const resident_view_ops_context<Alphabet>*>(state);
    if (required_length == 0u) {
        return scratch.data();
    }
    const sequence_size_t total = context.descriptor->bp_length;
    if constexpr (IsReversed
                  && !complementable_sequence_alphabet<Alphabet>) {
        throw std::invalid_argument(
            "reverse-complement encoding requires a complementable alphabet");
    }
    const sequence_size_t forward_from = IsReversed
        ? total - (bp_from + required_length)
        : bp_from;
    constexpr bool descending_write =
        (IsReversed && !WriteReversed) || (!IsReversed && WriteReversed);
    for (std::size_t index = 0u; index < required_length; ++index) {
        std::uint8_t rank = resident_rank_at(
            context, forward_from + index);
        if constexpr (IsReversed
                      && complementable_sequence_alphabet<Alphabet>) {
            rank = lazy_rank_stream_traits<Alphabet>::letter_complement(rank);
        }
        scratch[descending_write ? required_length - 1u - index : index] = rank;
    }
    return scratch.data();
}

template <sequence_alphabet Alphabet>
[[nodiscard]] sequence_size_t resident_bp_size(void const* state) noexcept {
    const auto& context =
        *static_cast<const resident_view_ops_context<Alphabet>*>(state);
    return context.descriptor->bp_length;
}

template <sequence_alphabet Alphabet>
[[nodiscard]] auto resident_probe(
        void const* state,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width) noexcept
    -> typename basic_lazy_sequence_view<Alphabet>::resident_probe_result {
    using view_type = basic_lazy_sequence_view<Alphabet>;
    using descriptor_type = typename view_type::resident_packed_region;
    const auto& context =
        *static_cast<const resident_view_ops_context<Alphabet>*>(state);
    const descriptor_type& source = *context.descriptor;
    if (requested_width != source.storage_unit_byte_width) {
        return std::nullopt;
    }
    const sequence_size_t length = to - from;
    if (length == 0u) {
        return descriptor_type{
            .units = nullptr,
            .unit_count = 0u,
            .bp_length = 0u,
            .storage_unit_byte_width = source.storage_unit_byte_width,
            .bits_per_symbol = source.bits_per_symbol,
            .leading_symbol_offset = 0u,
            .trailing_padding_letters = 0u,
            .bit_order = source.bit_order,
        };
    }

    const sequence_size_t letters_per_unit =
        source.storage_unit_byte_width * CHAR_BIT / source.bits_per_symbol;
    const sequence_size_t absolute_slot =
        source.leading_symbol_offset + from;
    const sequence_size_t first_unit = absolute_slot / letters_per_unit;
    const sequence_size_t leading = absolute_slot % letters_per_unit;
    const sequence_size_t occupied = leading + length;
    const sequence_size_t unit_count =
        occupied / letters_per_unit
        + static_cast<sequence_size_t>(occupied % letters_per_unit != 0u);
    if (first_unit > source.unit_count
        || unit_count > source.unit_count - first_unit
        || unit_count
                > static_cast<sequence_size_t>(
                    (std::numeric_limits<std::size_t>::max)())) {
        return std::nullopt;
    }
    const auto* units = static_cast<const std::uint8_t*>(source.units)
        + first_unit * source.storage_unit_byte_width;
    return descriptor_type{
        .units = units,
        .unit_count = static_cast<std::size_t>(unit_count),
        .bp_length = length,
        .storage_unit_byte_width = source.storage_unit_byte_width,
        .bits_per_symbol = source.bits_per_symbol,
        .leading_symbol_offset = static_cast<std::uint16_t>(leading),
        .trailing_padding_letters = static_cast<std::uint16_t>(
            unit_count * letters_per_unit - occupied),
        .bit_order = source.bit_order,
    };
}

[[nodiscard]] inline bool resident_worker_shareable(void const*) noexcept {
    return true;
}

template <sequence_alphabet Alphabet>
struct resident_adapter {
    using view_type = basic_lazy_sequence_view<Alphabet>;
    static inline const typename view_type::backend_ops ops{
        .bp_size = &resident_bp_size<Alphabet>,
        .ptr_at = {
            &resident_ptr_at<false, false, Alphabet>,
            &resident_ptr_at<false, true, Alphabet>,
            &resident_ptr_at<true, false, Alphabet>,
            &resident_ptr_at<true, true, Alphabet>,
        },
        .try_resident_packed = &resident_probe<Alphabet>,
        .copy_full_packed = nullptr,
        .worker_shareable = &resident_worker_shareable,
    };
};

template <sequence_alphabet Alphabet, class Unit>
[[nodiscard]] bioseq::sequences::rank_pack_result pack_view_region_into(
        basic_lazy_sequence_view<Alphabet> view,
        sequence_size_t from,
        sequence_size_t to,
        void* output,
        std::size_t unit_count) {
    constexpr std::size_t bits =
        lazy_rank_stream_traits<Alphabet>::logical_bits_per_symbol;
    using output_instance = sequence_custom_alphabet<Alphabet, Unit, bits>;
    using unit_sink = bioseq::sequences::fixed_rank_pack_unit_sink<Unit>;
    using packer = bioseq::sequences::checked_rank_pack_sink<
        output_instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        unit_sink>;

    auto units = std::span<Unit>(static_cast<Unit*>(output), unit_count);
    packer writer{unit_sink{units}};
    stream_transformed_packed<sequence_transform::identity>(
        view, from, to, writer);
    return writer.finish(to - from, unit_count);
}

}  // namespace lazy_view_detail

template <sequence_alphabet Alphabet>
class admitted_packed_region;

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint64_t packed_region_admission_bytes(
    sequence_size_t logical_length,
    std::uint8_t requested_width);

template <sequence_alphabet Alphabet>
[[nodiscard]] admitted_packed_region<Alphabet> materialize_packed_region(
    basic_lazy_sequence_view<Alphabet> view,
    sequence_size_t from,
    sequence_size_t to,
    std::uint8_t requested_width,
    bioseq::parallelism::MemoryPermit&& permit);

template <sequence_alphabet Alphabet>
class admitted_packed_region {
public:
    using view_type = basic_lazy_sequence_view<Alphabet>;
    using descriptor_type = typename view_type::resident_packed_region;

    admitted_packed_region(admitted_packed_region&&) noexcept = default;
    admitted_packed_region& operator=(admitted_packed_region&&) = delete;
    admitted_packed_region(admitted_packed_region const&) = delete;
    admitted_packed_region& operator=(admitted_packed_region const&) = delete;

    [[nodiscard]] descriptor_type const& descriptor() const noexcept {
        if (state_ == nullptr) {
            lazy_fragment_panic("access to moved admitted packed region");
        }
        return state_->descriptor_;
    }

    [[nodiscard]] view_type view() const noexcept {
        if (state_ == nullptr) {
            lazy_fragment_panic("view of moved admitted packed region");
        }
        using adapter = lazy_view_detail::resident_adapter<Alphabet>;
        return view_type{std::addressof(state_->view_context_),
                         std::addressof(adapter::ops)};
    }

#ifdef BIOSEQ_TEST_INDEXES
    [[nodiscard]] std::uint64_t test_admission_bytes() const noexcept {
        if (state_ == nullptr) {
            lazy_fragment_panic("admission query on moved packed region");
        }
        return state_->permit_.bytes();
    }

    [[nodiscard]] std::uintptr_t test_storage_address() const noexcept {
        if (state_ == nullptr) {
            lazy_fragment_panic("storage query on moved packed region");
        }
        return reinterpret_cast<std::uintptr_t>(state_->descriptor_.units);
    }
#endif

private:
    struct stable_state {
        bioseq::parallelism::MemoryPermit permit_;
        lazy_view_detail::aligned_packed_storage units_;
        descriptor_type descriptor_;
        lazy_view_detail::resident_view_ops_context<Alphabet> view_context_;

        stable_state(bioseq::parallelism::MemoryPermit&& permit,
                     std::size_t allocation_bytes,
                     descriptor_type descriptor)
            : permit_(std::move(permit)),
              units_(lazy_view_detail::aligned_packed_storage::allocate(
                  allocation_bytes)),
              descriptor_(descriptor),
              view_context_{std::addressof(descriptor_)} {
            descriptor_.units = units_.data();
        }
    };

    explicit admitted_packed_region(std::unique_ptr<stable_state> state) noexcept
        : state_(std::move(state)) {}

    template <sequence_alphabet OtherAlphabet>
    friend admitted_packed_region<OtherAlphabet> materialize_packed_region(
        basic_lazy_sequence_view<OtherAlphabet>,
        sequence_size_t,
        sequence_size_t,
        std::uint8_t,
        bioseq::parallelism::MemoryPermit&&);

    template <sequence_alphabet OtherAlphabet>
    friend std::uint64_t packed_region_admission_bytes(
        sequence_size_t,
        std::uint8_t);

    std::unique_ptr<stable_state> state_;
};

template <sequence_alphabet Alphabet>
[[nodiscard]] std::uint64_t packed_region_admission_bytes(
        sequence_size_t logical_length,
        std::uint8_t requested_width) {
    using owner_type = admitted_packed_region<Alphabet>;
    constexpr std::uint8_t bits = static_cast<std::uint8_t>(
        lazy_rank_stream_traits<Alphabet>::logical_bits_per_symbol);
    if (!(requested_width == 1u || requested_width == 2u
          || requested_width == 4u || requested_width == 8u)) {
        throw std::invalid_argument("invalid packed storage-unit width");
    }
    const sequence_size_t letters_per_unit =
        requested_width * CHAR_BIT / bits;
    const sequence_size_t unit_count =
        logical_length / letters_per_unit
        + static_cast<sequence_size_t>(
            logical_length % letters_per_unit != 0u);
    if (unit_count
            > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())
        || unit_count
                > (std::numeric_limits<std::size_t>::max)()
                    / requested_width) {
        throw std::overflow_error("packed materialization size overflow");
    }
    const std::uint64_t allocation_bytes =
        static_cast<std::uint64_t>(unit_count) * requested_width;
    if (allocation_bytes
            > (std::numeric_limits<std::uint64_t>::max)()
                - sizeof(typename owner_type::stable_state)) {
        throw std::overflow_error("packed materialization admission overflow");
    }
    return sizeof(typename owner_type::stable_state) + allocation_bytes;
}

enum class sequence_access_route : std::uint8_t {
    stream_symbols,
    borrow_resident,
    materialize_admitted,
};

template <sequence_alphabet Alphabet>
struct sequence_access_plan {
    using descriptor_type = typename basic_lazy_sequence_view<Alphabet>::
        resident_packed_region;

    sequence_access_requirement requirement{
        sequence_access_requirement::sequential_symbols};
    sequence_access_route route{sequence_access_route::stream_symbols};
    std::optional<descriptor_type> resident{};
    std::uint64_t admission_bytes{};
    sequence_size_t from{};
    sequence_size_t to{};
    std::uint8_t requested_width{};
};

template <sequence_alphabet Alphabet>
[[nodiscard]] sequence_access_plan<Alphabet> plan_sequence_access(
        basic_lazy_sequence_view<Alphabet> view,
        sequence_access_requirement requirement,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width = 0u) {
    if (from > to || to > view.bp_size()) {
        throw std::invalid_argument("invalid sequence access range");
    }
    sequence_access_plan<Alphabet> plan{
        .requirement = requirement,
        .route = sequence_access_route::stream_symbols,
        .resident = std::nullopt,
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
        if (plan.resident.has_value()
            && plan.resident->leading_symbol_offset == 0u) {
            plan.route = sequence_access_route::borrow_resident;
            return plan;
        }
        plan.resident.reset();
    }
    plan.route = sequence_access_route::materialize_admitted;
    plan.admission_bytes = packed_region_admission_bytes<Alphabet>(
        to - from, requested_width);
    return plan;
}

template <sequence_alphabet Alphabet>
[[nodiscard]] admitted_packed_region<Alphabet> materialize_packed_region(
        basic_lazy_sequence_view<Alphabet> view,
        sequence_size_t from,
        sequence_size_t to,
        std::uint8_t requested_width,
        bioseq::parallelism::MemoryPermit&& permit) {
    using owner_type = admitted_packed_region<Alphabet>;
    using descriptor_type = typename owner_type::descriptor_type;
    using bit_order_type =
        typename basic_lazy_sequence_view<Alphabet>::packed_bit_order;
    constexpr std::uint8_t bits = static_cast<std::uint8_t>(
        lazy_rank_stream_traits<Alphabet>::logical_bits_per_symbol);

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
        requested_width * CHAR_BIT / bits;
    const sequence_size_t unit_count =
        length / letters_per_unit
        + static_cast<sequence_size_t>(length % letters_per_unit != 0u);
    if (unit_count
            > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())
        || unit_count
                > (std::numeric_limits<std::size_t>::max)()
                    / requested_width) {
        throw std::overflow_error("packed materialization size overflow");
    }
    const std::size_t allocation_bytes =
        static_cast<std::size_t>(unit_count) * requested_width;
    const std::uint64_t admitted_bytes = packed_region_admission_bytes<Alphabet>(
        length, requested_width);
    if (!permit || permit.bytes() < admitted_bytes) {
        throw std::length_error(
            "packed materialization permit is smaller than retained storage");
    }

    const sequence_size_t trailing =
        unit_count * letters_per_unit - length;
    descriptor_type descriptor{
        .units = nullptr,
        .unit_count = static_cast<std::size_t>(unit_count),
        .bp_length = length,
        .storage_unit_byte_width = requested_width,
        .bits_per_symbol = bits,
        .leading_symbol_offset = 0u,
        .trailing_padding_letters = static_cast<std::uint16_t>(trailing),
        .bit_order = bit_order_type::least_significant_symbol_first,
    };
    auto state = std::make_unique<typename owner_type::stable_state>(
        std::move(permit), allocation_bytes, descriptor);

    const bool bulk_copied = from == 0u && to == view.bp_size()
        && view.try_copy_full_packed(
            std::span<std::uint8_t>{
                state->units_.data(), allocation_bytes},
            static_cast<std::size_t>(unit_count), requested_width);
    if (!bulk_copied) {
        bioseq::sequences::rank_pack_result packed{};
        switch (requested_width) {
            case 1u:
                packed = lazy_view_detail::pack_view_region_into<
                    Alphabet, std::uint8_t>(
                        view, from, to, state->units_.data(),
                        static_cast<std::size_t>(unit_count));
                break;
            case 2u:
                packed = lazy_view_detail::pack_view_region_into<
                    Alphabet, std::uint16_t>(
                        view, from, to, state->units_.data(),
                        static_cast<std::size_t>(unit_count));
                break;
            case 4u:
                packed = lazy_view_detail::pack_view_region_into<
                    Alphabet, std::uint32_t>(
                        view, from, to, state->units_.data(),
                        static_cast<std::size_t>(unit_count));
                break;
            case 8u:
                packed = lazy_view_detail::pack_view_region_into<
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
    (void)observe_sequence_payload_allocation_noexcept(
        view.payload_allocation_observer(),
        sequence_payload_allocation_kind::admitted_packed_region,
        length, length, static_cast<std::uint64_t>(allocation_bytes),
        admitted_bytes);
    return owner_type{std::move(state)};
}

static_assert(std::is_move_constructible_v<
              admitted_packed_region<seq::dna4>>);
static_assert(!std::is_move_assignable_v<
              admitted_packed_region<seq::dna4>>);
static_assert(!std::is_copy_constructible_v<
              admitted_packed_region<seq::dna4>>);

/// Construct a fragmented lazy wrapper from a scanner factory and invoke
/// `fn(lazy)`. The (non-movable) wrapper is held by reference inside the
/// callback and never escapes (DOC0263 §4.2 — replaces the broken
/// holder-returned-by-value design, consolidated-review C15).
template <class R, class ScannerFactory, class Fn>
inline void with_lazy_fragmented(ScannerFactory&& scanner_fn, Fn&& fn) {
    using S = std::invoke_result_t<ScannerFactory>;
    lazy_compact_sequence<R, S> lazy{std::forward<ScannerFactory>(scanner_fn)()};
    std::forward<Fn>(fn)(lazy);
}

/// DOC0263 §9.1/§9.2 — the aligner-facing encode path. Routes the encode
/// through the lazy wrapper's `encoded_iter<…>().ptr_at(...)` slot ring (§7)
/// so the slot ring is exercised by every block_aligner/ksw2 call site (NO
/// `std::vector<uint8_t>` ever materialises on the fragmented encoded path,
/// §1.1#7). `ptr_at` returns a pointer to `len` encoded bytes — a resident
/// slot for the §5 plain-forward zero-copy fast path, otherwise the caller's
/// `dst`. When it is a slot we copy into `dst` so the aligner's padded/
/// contiguous layout is preserved. `eit` is a named local so its slot ring
/// outlives the copy. One canonical facade helper is shared by every aligner
/// module (both `bioseq_alignment_hwy.block_aligner` and
/// `bioseq_alignment_hwy.ksw2` import this).
template <bool is_reversed, bool write_reversed = false,
          sequence_alphabet Alphabet>
[[gnu::always_inline]] inline void encode_region_via_ptr_at(
    basic_lazy_sequence_view<Alphabet> seq,
    std::span<std::uint8_t> dst,
    std::size_t bp_from, std::size_t bp_to)
{
    if (bp_from > bp_to) {
        lazy_fragment_panic("lazy sequence encoded region is inverted");
    }
    if (bp_from == bp_to) return;
    const std::size_t len = bp_to - bp_from;
    auto eit = seq.template encoded_iter<
        is_reversed, write_reversed, std::uint8_t, 2>();
    const std::uint8_t* p = eit.ptr_at(
        static_cast<sequence_size_t>(bp_from), len, dst);
    if (p != dst.data()) {
        std::memcpy(dst.data(), p, len * sizeof(std::uint8_t));
    }
}

}  // namespace bioseq::storage
