/**
 * @file storage/lazy_sequence_parts/owners.cppm
 * @brief Lazy readers for resident ranges and fragmented sequence storage.
 */
export module bioseq.storage.lazy_sequence:owners;

import std;
import bioseq.storage.lazy_sequence_view;
import bioseq.common.checked_arithmetic;
import bioseq.common.fragment_cache;
import bioseq.common.seqan_adapter;
import bioseq.storage.scanner;
import bioseq.storage.size;
import bioseq.sequences;

export namespace bioseq::storage {

template <class p_sequence_range, class p_scanner> struct lazy_sequence;

/** Pointer-sized adapter borrowing an existing resident compact range. */

template <class p_sequence_range, class p_scanner>
    requires(!p_scanner::is_fragmented())
struct lazy_sequence<p_sequence_range, p_scanner> {
    using range_type = p_sequence_range;
    using scanner_type = p_scanner;
    using traits = typename compact_sequence_range<p_sequence_range>::traits;
    using storage_unit = typename traits::storage_unit;

    static constexpr std::size_t k_fragment_cache_capacity = 1u;
    static constexpr std::size_t k_retained_fragment_limit = 1u;

    const compact_sequence_range<p_sequence_range>* range_ = nullptr;

    [[nodiscard]] [[gnu::always_inline]]
    explicit constexpr lazy_sequence(const compact_sequence_range<p_sequence_range>& range) noexcept
        : range_(&range) {}

    /// Returns the borrowed compact range for algorithms that read resident storage directly.
    [[nodiscard]] [[gnu::always_inline]]
    const compact_sequence_range<p_sequence_range>& underlying() const noexcept {
        return *range_;
    }

    // Sequence sizei and packed layout.
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t bp_size() const noexcept {
        return static_cast<sequence_size_t>(range_->bp_size());
    }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t size() const noexcept {
        return static_cast<sequence_size_t>(range_->size());
    }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t byte_size() const noexcept {
        return static_cast<sequence_size_t>(range_->byte_size());
    }
    [[nodiscard]] [[gnu::always_inline]]
    std::uint16_t padding() const noexcept {
        return static_cast<std::uint16_t>(range_->padding());
    }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t fragment_count() const noexcept {
        return 1;
    }

    // Iterators from the underlying compact range.
    using iterator_impl = typename compact_sequence_range<p_sequence_range>::iterator;
    [[nodiscard]] [[gnu::always_inline]] iterator_impl cbegin() const noexcept {
        return range_->cbegin();
    }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl cend() const noexcept {
        return range_->cend();
    }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl begin() const noexcept {
        return range_->cbegin();
    }
    [[nodiscard]] [[gnu::always_inline]] iterator_impl end() const noexcept {
        return range_->cend();
    }

    template <iterator_mode p_mode, class p_iterator>
    [[nodiscard]] [[gnu::always_inline]] auto iter(const p_iterator& iterator) const noexcept {
        return range_->template iter<p_mode>(iterator);
    }
    template <iterator_mode p_mode>
    [[nodiscard]] [[gnu::always_inline]] auto iterable() const noexcept {
        return range_->template iterable<p_mode>();
    }

    template <class p_iterator>
    [[nodiscard]] [[gnu::always_inline]] std::string
    to_string(p_iterator begin, p_iterator end, bool split_units = false) const {
        return range_->to_string(begin, end, split_units);
    }

    // Packed views with another storage-unit type, for resident storage only.
    template <class p_storage_unit>
    [[nodiscard]] [[gnu::always_inline]] auto bit_cast_to_storage_unit() const noexcept {
        return range_->template bit_cast_to_storage_unit<p_storage_unit>();
    }

    // Rank copies used by  alignment code.
    template <bool p_is_reversed, bool p_write_reversed = false>
    [[gnu::always_inline]]
    void copy_ranks(std::span<std::uint8_t> destination, std::size_t from, std::size_t to) const {
        if (from > to || to > bp_size() || destination.size() < to - from) {
            abort_lazy_sequence_access("resident lazy rank range is invalid");
        }
        if (from == to) {
            return;
        }
        range_->template to_encoded_into<p_is_reversed, p_write_reversed>(destination, from, to);
    }

    /** Rank-window reader that writes resident ranks into caller scratch. */
    template <bool p_is_reversed,
              bool p_write_reversed,
              class p_stored_type,
              std::size_t p_max_loaded_slots = 2>
    struct rank_window_reader {
        const lazy_sequence* parent_ = nullptr;
        explicit rank_window_reader(const lazy_sequence* parent) noexcept : parent_(parent) {}
        [[nodiscard]] const p_stored_type* read_at(sequence_size_t bp_from,
                                                   std::size_t required_length,
                                                   std::span<p_stored_type> scratch) const {
            if (bp_from > parent_->bp_size() || required_length > parent_->bp_size() - bp_from ||
                scratch.size() < required_length ||
                bp_from > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
                abort_lazy_sequence_access("resident lazy rank range is invalid");
            }
            if (required_length == 0u) {
                return scratch.data();
            }
            static_assert(sizeof(p_stored_type) == 1,
                          "no-fragment rank_reader::read_at expects a 1-byte encoded type");
            parent_->range_->template to_encoded_into<p_is_reversed, p_write_reversed>(
                std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(scratch.data()),
                                        required_length),
                static_cast<std::size_t>(bp_from),
                static_cast<std::size_t>(bp_from) + required_length);
            return scratch.data();
        }
    };
    template <bool p_is_reversed,
              bool p_write_reversed,
              class p_stored_type,
              std::size_t p_max_loaded_slots = 2>
    [[nodiscard]] auto rank_reader() const noexcept {
        return rank_window_reader<p_is_reversed,
                                  p_write_reversed,
                                  p_stored_type,
                                  p_max_loaded_slots>(this);
    }

    // Both read_cptr overloads borrow the existing resident words.
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it) const noexcept {
        return compact_sequence_range<p_sequence_range>::read_cptr(it);
    }
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it,
                                  std::span<storage_unit> /*scratch unused*/,
                                  std::size_t /*required_length unused*/) const noexcept {
        return compact_sequence_range<p_sequence_range>::read_cptr(it);
    }
};
/** Owns a fragmented scanner and a bounded cache of its loaded fragments. */

template <class p_sequence_range, class p_scanner>
    requires(p_scanner::is_fragmented())
struct lazy_sequence<p_sequence_range, p_scanner> {
    using range_type = p_sequence_range;
    using scanner_type = p_scanner;
    using traits = typename compact_sequence_range<p_sequence_range>::traits;
    using storage_unit = typename traits::storage_unit;
    using fragment_type = sequence_fragment<p_sequence_range>;

    static constexpr std::size_t k_fragment_storage_units = p_scanner::fragment_storage_units;
    static constexpr std::size_t k_fragment_cache_capacity = 16u;
    static constexpr std::size_t k_retained_fragment_limit = 1u;
    static_assert(k_fragment_cache_capacity > k_retained_fragment_limit,
                  "buffer capacity must exceed the post-eviction target count");

    static constexpr std::size_t k_letters_per_unit = traits::letters_per_unit;
    static constexpr std::size_t k_bits_per_symbol = traits::bits_per_char;
    static_assert(k_fragment_storage_units <=
                      (std::numeric_limits<std::size_t>::max)() / k_letters_per_unit,
                  "fragment rank capacity must fit size_t");
    static constexpr std::size_t k_fragment_logical_capacity =
        k_fragment_storage_units * k_letters_per_unit;

    // Iterators and rank readers may hold cache-slot references, so the
    // scanner owner has a stable address for its complete lifetime.
    lazy_sequence(const lazy_sequence&) = delete;
    lazy_sequence& operator=(const lazy_sequence&) = delete;
    lazy_sequence(lazy_sequence&&) = delete;
    lazy_sequence& operator=(lazy_sequence&&) = delete;

    explicit lazy_sequence(p_scanner&& scanner) noexcept : scanner_(std::move(scanner)) {}
    ~lazy_sequence() = default;

    // Sequence sizes and packed layout.
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t bp_size() const noexcept {
        return scanner_.bp_size();
    }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t size() const noexcept {
        return scanner_.storage_unit_count();
    }
    [[nodiscard]] [[gnu::always_inline]]
    sequence_size_t byte_size() const noexcept {
        return scanner_.storage_byte_count();
    }
    [[nodiscard]] [[gnu::always_inline]]
    std::uint16_t padding() const noexcept {
        return static_cast<std::uint16_t>(scanner_.trailing_padding());
    }
    [[nodiscard]] [[gnu::always_inline]]
    std::size_t fragment_count() const noexcept {
        return scanner_.fragment_count();
    }
    [[nodiscard]] const p_scanner& scanner() const noexcept {
        return scanner_;
    }

    /** Packed-word iterator holding its owner and global word index. */
    struct iterator_impl {
        const lazy_sequence* parent_ = nullptr;
        sequence_size_t global_unit_ = 0;

        [[gnu::always_inline]] iterator_impl& operator++() noexcept {
            return *this += 1u;
        }
        [[gnu::always_inline]] iterator_impl& operator+=(sequence_size_t count) noexcept {
            if (parent_ == nullptr || global_unit_ > parent_->size() ||
                count > parent_->size() - global_unit_) {
                abort_lazy_sequence_access("packed sequence iterator advanced past end");
            }
            global_unit_ += count;
            return *this;
        }

        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t current_fragment_index() const noexcept {
            return global_unit_ / k_fragment_storage_units;
        }
        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t current_fragment_unit_offset() const noexcept {
            return global_unit_ % k_fragment_storage_units;
        }
        [[nodiscard]] [[gnu::always_inline]]
        sequence_size_t global_unit_index() const noexcept {
            return global_unit_;
        }

        [[nodiscard]] [[gnu::always_inline]]
        friend bool operator==(const iterator_impl& left, const iterator_impl& right) noexcept {
            return left.parent_ == right.parent_ && left.global_unit_ == right.global_unit_;
        }
        [[nodiscard]] [[gnu::always_inline]]
        friend std::strong_ordering operator<=>(const iterator_impl& left,
                                                const iterator_impl& right) noexcept {
            if (left.parent_ != right.parent_) {
                abort_lazy_sequence_access("lazy iterator: cross-parent comparison");
            }
            return left.global_unit_ <=> right.global_unit_;
        }
    };

    [[nodiscard]] iterator_impl cbegin() const noexcept {
        return iterator_impl{this, 0};
    }
    [[nodiscard]] iterator_impl cend() const noexcept {
        return iterator_impl{this, size()};
    }
    [[nodiscard]] iterator_impl begin() const noexcept {
        return cbegin();
    }
    [[nodiscard]] iterator_impl end() const noexcept {
        return cend();
    }

    // Loading and releasing cached fragments.
    [[nodiscard]] const fragment_type& ensure_fragment(std::size_t fragment_number) const {
        if (fragment_number >= scanner_.fragment_count()) {
            abort_lazy_sequence_access("fragment index is out of range");
        }
        auto key_of = [](const fragment_type& fragment) noexcept {
            return fragment.fragment_index;
        };
        if (auto found = fragments_.find(fragment_number, key_of); found.has_value()) {
            return fragments_.slot_at(found->first);
        }
        load_fragment(fragment_number);
        auto loaded_fragment = fragments_.find(fragment_number, key_of);
        if (!loaded_fragment.has_value()) {
            abort_lazy_sequence_access("lazy: ensure_fragment failed to load fragment");
        }
        return fragments_.slot_at(loaded_fragment->first);
    }

    void evict_old_fragments() const {
        while (fragments_.valid_count() > k_retained_fragment_limit) {
            fragments_.evict_oldest([this](fragment_type& evicted) noexcept {
                scanner_.release_fragment(evicted.fragment_index);
            });
        }
    }

    /**
     * Reads packed words starting at it.
     *
     * A read within one fragment borrows its data until a later read evicts it.
     * A read crossing fragments copies into scratch. The scratch span  must hold
     * required_length words even when the returned pointer borrows a fragment
     */
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it,
                                  std::span<storage_unit> scratch,
                                  std::size_t required_length) const {
        if (it.parent_ != this) {
            abort_lazy_sequence_access("read_cptr: iterator's parent doesn't match this wrapper");
        }
        if (it.global_unit_ > size() || required_length > size() - it.global_unit_ ||
            scratch.size() < required_length) {
            abort_lazy_sequence_access("packed sequence read range is invalid");
        }
        if (required_length == 0u) {
            return scratch.data();
        }
        const sequence_size_t fragment_index = it.global_unit_ / k_fragment_storage_units;
        if (fragment_index >
            static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
            abort_lazy_sequence_access("packed sequence fragment index is unsupported");
        }
        const std::size_t starting_fragment_number = static_cast<std::size_t>(fragment_index);
        const std::size_t in_frag_offset =
            static_cast<std::size_t>(it.global_unit_ % k_fragment_storage_units);

        const fragment_type& starting_fragment = ensure_fragment(starting_fragment_number);
        // Borrow, directly when the requested words fit in this fragment.
        if (in_frag_offset <= starting_fragment.data.size() &&
            required_length <= starting_fragment.data.size() - in_frag_offset) {
            return starting_fragment.data.data() + in_frag_offset;
        }
        // Copy across fragment boundaries into caller scratch.
        std::size_t copied = 0;
        std::size_t current_unit_offset = in_frag_offset;
        std::size_t current_fragment_number = starting_fragment_number;
        while (copied < required_length) {
            const fragment_type& current_fragment = ensure_fragment(current_fragment_number);
            const std::size_t available_units = current_fragment.data.size() - current_unit_offset;
            const std::size_t to_copy = std::min(available_units, required_length - copied);
            std::memcpy(scratch.data() + copied,
                        current_fragment.data.data() + current_unit_offset,
                        to_copy * sizeof(storage_unit));
            copied += to_copy;
            current_fragment_number += 1;
            current_unit_offset = 0;
            if (current_fragment_number >= scanner_.fragment_count() && copied < required_length) {
                abort_lazy_sequence_access("packed read extends past the sequence end");
            }
        }
        evict_old_fragments();
        return scratch.data();
    }

    // Borrow a pointer into one fragment, valid until that fragment is evicted.
    [[nodiscard]] [[gnu::always_inline]]
    const storage_unit* read_cptr(const iterator_impl& it) const {
        if (it.parent_ != this || it.global_unit_ >= size()) {
            abort_lazy_sequence_access("packed sequence iterator is out of range");
        }
        const sequence_size_t fragment_index = it.global_unit_ / k_fragment_storage_units;
        if (fragment_index >
            static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
            abort_lazy_sequence_access("packed sequence fragment index is unsupported");
        }
        const std::size_t fragment_number = static_cast<std::size_t>(fragment_index);
        const std::size_t in_frag_offset =
            static_cast<std::size_t>(it.global_unit_ % k_fragment_storage_units);
        const fragment_type& fragment = ensure_fragment(fragment_number);
        return fragment.data.data() + in_frag_offset;
    }

    // Forward rank iterator across fagment boundaries. Stops at bp_size(),
    // excluding final padding. The first rank occupies each word's low bits.
    struct logical_rank_iterator_impl {
        const lazy_sequence* parent_ = nullptr;
        sequence_size_t global_rank_ = 0;

        [[nodiscard]] storage_unit operator*() const {
            if (parent_ == nullptr || global_rank_ >= parent_->bp_size()) {
                abort_lazy_sequence_access("logical-rank iterator dereferenced at end");
            }
            const sequence_size_t fragment_index = global_rank_ / k_fragment_logical_capacity;
            if (fragment_index >
                static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
                abort_lazy_sequence_access("logical-rank fragment index is unsupported");
            }
            const std::size_t frag = static_cast<std::size_t>(fragment_index);
            const fragment_type& fragment = parent_->ensure_fragment(frag);
            const std::size_t bp_in_frag =
                static_cast<std::size_t>(global_rank_ % k_fragment_logical_capacity);
            const std::size_t unit_in_frag = bp_in_frag / k_letters_per_unit;
            const std::size_t letter_in_unit = bp_in_frag % k_letters_per_unit;
            const storage_unit packed_unit = fragment.data[unit_in_frag];
            return static_cast<storage_unit>((packed_unit >> (letter_in_unit * k_bits_per_symbol)) &
                                             traits::char_mask);
        }
        [[gnu::always_inline]] logical_rank_iterator_impl& operator++() noexcept {
            if (parent_ == nullptr || global_rank_ >= parent_->bp_size()) {
                abort_lazy_sequence_access("logical-rank iterator advanced past end");
            }
            ++global_rank_;
            return *this;
        }
        [[nodiscard]] [[gnu::always_inline]]
        friend bool operator==(const logical_rank_iterator_impl& left,
                               const logical_rank_iterator_impl& right) noexcept {
            return left.parent_ == right.parent_ && left.global_rank_ == right.global_rank_;
        }
    };

    [[nodiscard]] logical_rank_iterator_impl rank_begin() const noexcept {
        return logical_rank_iterator_impl{this, 0};
    }
    [[nodiscard]] logical_rank_iterator_impl rank_end() const noexcept {
        return logical_rank_iterator_impl{this, bp_size()};
    }

    // Match the compact range's rank-iteration interface  so the scalar
    // minimizer kernel can also read fragmented sequences.
    template <iterator_mode p_mode, class p_iterator>
    [[nodiscard]] logical_rank_iterator_impl iter(const p_iterator& iterator) const noexcept {
        static_assert(p_mode == iterator_mode::logical_rank,
                      "fragmented lazy iter: only logical-rank iteration is supported "
                      "the minimizer kernel can consume ranks; alphabet-symbol mode is not "
                      "exercised on the fragmented path.");
        if (iterator.global_unit_ >= size()) {
            return logical_rank_iterator_impl{this, bp_size()};
        }
        const auto logical_rank = bioseq::common::checked_mul(
            iterator.global_unit_, static_cast<sequence_size_t>(k_letters_per_unit));
        if (!logical_rank.has_value() || *logical_rank > bp_size()) {
            abort_lazy_sequence_access("packed iterator cannot be mapped to a logical rank");
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
    template <iterator_mode p_mode>
    [[nodiscard]] logical_rank_range_view iterable() const noexcept {
        static_assert(p_mode == iterator_mode::logical_rank,
                      "fragmented lazy iterable supports logical-rank mode only");
        return logical_rank_range_view{rank_begin(), rank_end()};
    }

    /**
     * Copies the logical rank interval [from, to) into destination.
     *
     * With p_is_reversed, coordinates refer to the full reverse complement.
     * p_write_reversed reverses the chosen output interval again. The destination
     * must have room for to - from ranks.
     */
    template <bool p_is_reversed, bool p_write_reversed = false>
    void copy_ranks(std::span<std::uint8_t> destination, std::size_t from, std::size_t to) const {
        if (from > to || to > bp_size() || destination.size() < to - from) {
            abort_lazy_sequence_access("fragmented lazy rank range is invalid");
        }
        if (from == to) {
            return;
        }
        const std::size_t length = to - from;
        auto reader = rank_reader<p_is_reversed, p_write_reversed, std::uint8_t, 2>();
        const std::uint8_t* ranks =
            reader.read_at(static_cast<sequence_size_t>(from), length, destination);
        if (ranks != destination.data()) {
            std::memcpy(destination.data(), ranks, length);
        }
    }

    /** Rejects packed reinterpretation because a word can cross fragment boundaries. */
    template <class p_storage_unit> auto bit_cast_to_storage_unit() const noexcept {
        static_assert(sizeof(p_storage_unit) == 0,
                      "lazy_sequence::bit_cast_to_storage_unit is unavailable on the "
                      "fragmented specialisation (lanes spread across fragments). Gate the "
                      "bit-cast path via `if constexpr (p_scanner::is_fragmented())` and "
                      "take the fragmented path.");
    }

    /**
     * Caches decoded forward ranks for a bounded number of fragments.
     *
     * Plain forward reads within one cached fragment borrow that slot. Other reads
     * copy  to caller scratch and apply the requested orientation. Borrowed slots
     * can be evicted by later reads and are freed when the reader is destroyed.
     */
    template <bool p_is_reversed,
              bool p_write_reversed,
              class p_stored_type,
              std::size_t p_max_loaded_slots = 2>
    struct rank_window_reader {
        static_assert(p_max_loaded_slots >= 1u);
        struct encoded_slot {
            p_stored_type* bytes = nullptr; // malloc-allocated forward ranks
            std::size_t fragment_no = std::numeric_limits<std::size_t>::max();
            std::size_t bp_count = 0;
        };

        const lazy_sequence* parent_ = nullptr;
        mutable bioseq::common::fragment_cache<encoded_slot, p_max_loaded_slots + 1> slots_{};

        explicit rank_window_reader(const lazy_sequence* parent) noexcept : parent_(parent) {}
        rank_window_reader(const rank_window_reader&) = delete;
        rank_window_reader& operator=(const rank_window_reader&) = delete;
        rank_window_reader(rank_window_reader&&) = delete;
        rank_window_reader& operator=(rank_window_reader&&) = delete;
        ~rank_window_reader() {
            // Free every resident slot's bytes.
            for (std::size_t slot_index = 0; slot_index < slots_.k_capacity; ++slot_index) {
                slots_.evict_at(slot_index, [](encoded_slot& slot) noexcept {
                    std::free(slot.bytes);
                    slot.bytes = nullptr;
                });
            }
        }

        // Cache forward ranks without complementing them. read_at applies the
        // requested orientation while copying, so cached data stays reusable.
        encoded_slot& ensure_rank_slot(std::size_t frag) const {
            auto key_of = [](const encoded_slot& slot) noexcept { return slot.fragment_no; };
            if (auto found = slots_.find(frag, key_of); found.has_value()) {
                return slots_.slot_at(found->first);
            }
            const fragment_type& fragment = parent_->ensure_fragment(frag);
            const std::size_t bp_count = static_cast<std::size_t>(fragment.logical_length);
            const auto allocation_bytes =
                bioseq::common::checked_mul(bp_count, sizeof(p_stored_type));
            if (!allocation_bytes.has_value()) {
                abort_lazy_sequence_access("rank-window allocation size is unsupported");
            }
            p_stored_type* bytes = static_cast<p_stored_type*>(std::malloc(*allocation_bytes));
            if (bytes == nullptr) {
                abort_lazy_sequence_access("rank-window slot allocation failed");
            }
            for (std::size_t bp = 0; bp < bp_count; ++bp) {
                const std::size_t unit_in_frag = bp / k_letters_per_unit;
                const std::size_t letter_in_unit = bp % k_letters_per_unit;
                const storage_unit packed_unit = fragment.data[unit_in_frag];
                const storage_unit rank = static_cast<storage_unit>(
                    (packed_unit >> (letter_in_unit * k_bits_per_symbol)) & traits::char_mask);
                bytes[bp] = static_cast<p_stored_type>(rank);
            }
            auto [slot_index, generation] = slots_.push(encoded_slot{bytes, frag, bp_count},
                                                        [](encoded_slot& evicted) noexcept {
                                                            std::free(evicted.bytes);
                                                            evicted.bytes = nullptr;
                                                        });
            (void)generation;
            // Keep at most p_max_loaded_slots decoded fragments after insertion.
            while (slots_.valid_count() > p_max_loaded_slots) {
                slots_.evict_oldest([](encoded_slot& evicted) noexcept {
                    std::free(evicted.bytes);
                    evicted.bytes = nullptr;
                });
            }
            return slots_.slot_at(slot_index);
        }

        // bp_from uses reverse-complement coordinates when p_is_reversed is set.
        // A plain forward read within one jlot borrows that slot Other reads return
        // scratch.data(), with p_write_reversed controlling the final output order.
        [[nodiscard]]
        const p_stored_type* read_at(sequence_size_t bp_from,
                                     std::size_t required_length,
                                     std::span<p_stored_type> scratch) const {
            if (bp_from > parent_->bp_size() || required_length > parent_->bp_size() - bp_from ||
                scratch.size() < required_length) {
                abort_lazy_sequence_access("fragmented lazy rank range is invalid");
            }
            if (required_length == 0u) {
                return scratch.data();
            }
            if (parent_->bp_size() >
                static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
                abort_lazy_sequence_access("rank-window coordinates exceed size_t");
            }
            const std::size_t total = static_cast<std::size_t>(parent_->bp_size());
            // Convert  reverse-complement coordinates to the forward source interval.
            const std::size_t forward_from =
                p_is_reversed ? (total - (static_cast<std::size_t>(bp_from) + required_length))
                              : static_cast<std::size_t>(bp_from);
            constexpr bool k_is_descending_write =
                (p_is_reversed && !p_write_reversed) || (!p_is_reversed && p_write_reversed);

            // Borrow a decoded slot when a plain forward read fits inside it.
            if constexpr (!p_is_reversed && !p_write_reversed) {
                const std::size_t fragment_bp =
                    static_cast<std::size_t>(parent_->k_fragment_logical_capacity);
                const std::size_t cur_frag = forward_from / fragment_bp;
                const std::size_t in_frag_bp = forward_from % fragment_bp;
                const encoded_slot& slot = ensure_rank_slot(cur_frag);
                if (in_frag_bp + required_length <= slot.bp_count) {
                    return slot.bytes + in_frag_bp;
                }
            }
            // Gather forward ranks a, fragment at a time, applying complement and
            // output order while copying into scratch.
            const std::size_t fragment_bp =
                static_cast<std::size_t>(parent_->k_fragment_logical_capacity);
            std::size_t gathered = 0;
            std::size_t source_bp = forward_from;
            while (gathered < required_length) {
                const std::size_t frag = source_bp / fragment_bp;
                const std::size_t in_frag = source_bp % fragment_bp;
                const encoded_slot& slot = ensure_rank_slot(frag);
                const std::size_t available_ranks = slot.bp_count - in_frag;
                const std::size_t to_copy = std::min(available_ranks, required_length - gathered);
                for (std::size_t rank_index = 0; rank_index < to_copy; ++rank_index) {
                    const p_stored_type raw_rank = slot.bytes[in_frag + rank_index];
                    const p_stored_type letter =
                        p_is_reversed ? static_cast<p_stored_type>(traits::letter_complement(
                                            static_cast<storage_unit>(raw_rank)))
                                      : raw_rank;
                    const std::size_t output_index = gathered + rank_index;
                    scratch[k_is_descending_write ? (required_length - 1 - output_index)
                                                  : output_index] = letter;
                }
                gathered += to_copy;
                source_bp += to_copy;
                if (gathered < required_length && source_bp >= total) {
                    abort_lazy_sequence_access("rank read extends past the sequence end");
                }
            }
            return scratch.data();
        }

        [[nodiscard]] p_stored_type operator[](sequence_size_t bp_index) const {
            p_stored_type rank{};
            std::span<p_stored_type> rank_span(&rank, 1);
            const p_stored_type* ranks = read_at(bp_index, 1, rank_span);
            return ranks[0];
        }
    };

    template <bool p_is_reversed,
              bool p_write_reversed,
              class p_stored_type,
              std::size_t p_max_loaded_slots = 2>
    [[nodiscard]] auto rank_reader() const noexcept {
        return rank_window_reader<p_is_reversed,
                                  p_write_reversed,
                                  p_stored_type,
                                  p_max_loaded_slots>(this);
    }

  private:
    mutable p_scanner scanner_;
    mutable bioseq::common::fragment_cache<fragment_type, k_fragment_cache_capacity> fragments_{};

    void load_fragment(std::size_t fragment_number) const {
        const auto logical_offset =
            bioseq::common::checked_mul(static_cast<std::uint64_t>(fragment_number),
                                        static_cast<std::uint64_t>(k_fragment_logical_capacity));
        if (!logical_offset.has_value() || *logical_offset > scanner_.bp_size()) {
            abort_lazy_sequence_access("fragment logical offset is invalid");
        }
        fragment_type fragment;
        fragment.data = scanner_.load_fragment(fragment_number);
        fragment.fragment_index = fragment_number;
        fragment.logical_offset = *logical_offset;
        const bool is_last = fragment_number == scanner_.fragment_count() - 1u;
        fragment.logical_length = is_last
                                      ? (scanner_.bp_size() - fragment.logical_offset)
                                      : static_cast<sequence_size_t>(k_fragment_logical_capacity);
        fragment.trailing_padding =
            is_last ? static_cast<std::uint16_t>(scanner_.trailing_padding()) : std::uint16_t{0};
        const auto fragment_capacity =
            bioseq::common::checked_mul(static_cast<std::uint64_t>(fragment.data.size()),
                                        static_cast<std::uint64_t>(k_letters_per_unit));
        if (!fragment_capacity.has_value() || fragment.data.empty() ||
            fragment.data.size() > k_fragment_storage_units ||
            (!is_last && fragment.data.size() != k_fragment_storage_units) ||
            fragment.trailing_padding >= k_letters_per_unit ||
            fragment.logical_length > *fragment_capacity ||
            *fragment_capacity - fragment.logical_length != fragment.trailing_padding) {
            abort_lazy_sequence_access("scanner returned invalid fragment geometry");
        }
        fragments_.push(fragment, [this](fragment_type& evicted) noexcept {
            scanner_.release_fragment(evicted.fragment_index);
        });
        evict_old_fragments();
    }
};

/** Helpers that construct lazy readers for a caller or callback. */

/// Returns a small lazy adapter borrowing an in-memory compact range.
template <class p_sequence_range>
[[nodiscard]] [[gnu::always_inline]]
inline lazy_sequence<p_sequence_range,
                     resident_sequence_scanner<
                         typename compact_sequence_range<p_sequence_range>::traits::storage_unit>>
make_resident_lazy_sequence(const compact_sequence_range<p_sequence_range>& range) noexcept {
    return lazy_sequence<p_sequence_range,
                         resident_sequence_scanner<typename compact_sequence_range<
                             p_sequence_range>::traits::storage_unit>>{range};
}

/// Calls callback with a temporary lazy adapter borrowing range.
/// The adapter is valid  only during the callback.
template <class p_sequence_range, class p_callback>
[[gnu::always_inline]]
inline void with_resident_lazy_sequence(const compact_sequence_range<p_sequence_range>& range,
                                        p_callback&& callback) {
    lazy_sequence<p_sequence_range,
                  resident_sequence_scanner<
                      typename compact_sequence_range<p_sequence_range>::traits::storage_unit>>
        lazy_sequence_owner{range};
    std::forward<p_callback>(callback)(lazy_sequence_owner);
}

/**
 * Reports the scanner's in-memory mode when it exposes that query.
 *
 * Returns true for resident ranges and for scanners without is_in_memory_mode().
 * This is a source hint. It does not prove that a fragmented sequence can be
 * borrowed as one contiguous packed span.
 */
template <class p_sequence_range, class p_scanner>
[[nodiscard]] inline bool
has_resident_sequence_bytes(const lazy_sequence<p_sequence_range, p_scanner>& sequence) noexcept {
    if constexpr (p_scanner::is_fragmented()) {
        if constexpr (requires(const p_scanner& scanner) {
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

/** Returns true for resident adapters. Callers must keep their borrowed range alive during worker use. */
template <class p_sequence_range, class p_scanner>
[[nodiscard]] inline bool
is_worker_shareable(const lazy_sequence<p_sequence_range, p_scanner>&) noexcept {
    return !p_scanner::is_fragmented();
}

/// Constructs a fragmented sanner owner at a fixed address for one callback.
template <class p_sequence_range, class p_scanner_factory, class p_callback>
inline void with_fragmented_lazy_sequence(p_scanner_factory&& scanner_factory,
                                          p_callback&& callback) {
    using scanner_type = std::invoke_result_t<p_scanner_factory>;
    lazy_sequence<p_sequence_range, scanner_type> lazy_sequence_owner{
        std::forward<p_scanner_factory>(scanner_factory)()};
    std::forward<p_callback>(callback)(lazy_sequence_owner);
}

} // namespace bioseq::storage
