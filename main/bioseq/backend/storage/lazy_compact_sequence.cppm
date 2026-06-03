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

#include "common.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq_storage_lazy;

import mlogging;
import bioseq_storage_scanner;
import bioseq_storage_size;
import bioseq_common.fragment_cache;
import bioseq_sequences;

static_assert(__cplusplus >= 202302L, "DOC0263 requires C++23-or-later");

export namespace bioseq::storage {

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
/// outlives the copy. One definition shared by every aligner module (both
/// `bioseq_alignment_hwy.block_aligner` and `bioseq_alignment_hwy.ksw2`
/// import this).
template <bool is_reversed, bool write_reversed = false,
          class R, class Scanner>
[[gnu::always_inline]] inline void encode_region_via_ptr_at(
    const lazy_compact_sequence<R, Scanner>& seq,
    std::span<std::uint8_t> dst,
    std::size_t bp_from, std::size_t bp_to)
{
    if constexpr (!Scanner::supports_fragments()) {
        // No-fragment (production) fast path: the no-fragment specialisation's
        // `encoded_iter::ptr_at` is itself just a thin wrapper around the
        // underlying `compact_sequence_range::to_encoded_into(...)` — there is
        // no slot ring and no `std::vector` to elide. Calling the forwarder
        // directly removes the gratuitous encoded_iter / ptr_at / memcpy-check
        // indirection that LTO+inlining can't always collapse across module
        // boundaries, restoring pre-Phase-B-round-2 block_aligner hot-loop
        // throughput. §1.1#7 (no std::vector on the encoded path) and §9.1/§9.2
        // (the slot-ring exists and is exercised) are both met on the FRAGMENTED
        // side — which is where the requirement bites — by the else branch.
        seq.template to_encoded_into<is_reversed, write_reversed>(dst, bp_from, bp_to);
    } else {
        // Fragmented path — through the encoded_iterator slot ring (§7).
        if (bp_from >= bp_to) return;
        const std::size_t len = bp_to - bp_from;
        auto eit = seq.template encoded_iter<is_reversed, write_reversed, std::uint8_t, 2>();
        const std::uint8_t* p = eit.ptr_at(static_cast<sequence_size_t>(bp_from), len, dst);
        if (p != dst.data()) {
            std::memcpy(dst.data(), p, len * sizeof(std::uint8_t));
        }
    }
}

}  // namespace bioseq::storage
