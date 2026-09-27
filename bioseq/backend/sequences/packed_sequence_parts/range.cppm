/**
 * @file range.cppm
 * @brief Packed and unpacked sequence range implementations.
 */
module;

#include <libassert/assert.hpp>

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <valarray>
#include <vector>
#include <algorithm>
#include <limits>

#include <boost/integer.hpp>
#include <boost/iterator/iterator_adaptor.hpp>

export module bioseq.sequences:packed_sequence_range;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.templating;
import bioseq.sequences.rank_pack_core;
import :packed_sequence_core;
import :packed_sequence_packing;

export {

/**
 * @brief Supplies packed-symbol iteration and conversion to derived ranges.
 * @tparam p_derived_range Concrete range exposing packed-unit iterators.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_packed_unit Packed storage word type.
 * @tparam p_bits_per_character Stored rank width, or zero for one rank per unit.
 */
template<typename p_derived_range,
         typename p_alphabet,
         typename p_packed_unit,
         std::size_t p_bits_per_character>
struct compact_sequence_range_base {

    using self_type = compact_sequence_range_base<
        p_derived_range, p_alphabet, p_packed_unit, p_bits_per_character>;

    /** @brief Compile-time alphabet and packed-unit geometry. */
    struct traits {
        using alphabet_type = p_alphabet;
        using storage_unit = p_packed_unit;

        static constexpr std::size_t bits_per_char =
            p_bits_per_character == 0
                ? sizeof(storage_unit) * 8
                : p_bits_per_character;
        static constexpr size_t unit_width = sizeof(storage_unit) * 8;
        static constexpr size_t letters_per_unit = unit_width / bits_per_char;
        static constexpr size_t unit_used_bits = letters_per_unit * bits_per_char;
        static constexpr size_t unit_leftover_bits = unit_width - unit_used_bits;

        static constexpr storage_unit char_mask = (static_cast<storage_unit>(1) << bits_per_char) - 1;
        static constexpr size_t rev_char_mask = char_mask << (bits_per_char * (letters_per_unit - 1));
        // unit_used_bits can equal unit_width (when the alphabet packs exactly
        // — e.g. dna4 into uint8). `1 << unit_width` on a storage_unit-wide
        // integer is UB, so branch on the leftover-bits case.
        static constexpr storage_unit used_storage_mask =
            (unit_leftover_bits == 0)
                ? static_cast<storage_unit>(~static_cast<storage_unit>(0))
                : static_cast<storage_unit>((static_cast<storage_unit>(1) << unit_used_bits) - 1);

        static constexpr size_t alphabet_size =
            seqan3::alphabet_size<p_alphabet>;
        static constexpr size_t alphabet_size_n = alphabet_size + 1;

        using letter_unit = typename boost::uint_t<seqan3::detail::ceil_log2(alphabet_size)>::least; // smallest type to hold single letter of this alphabet. This is useful when you want to hold each letter separately for processing
        static constexpr size_t bits_per_char_unit = sizeof(letter_unit) * 8; // width in bits of letter_unit. bits_per_char_unit should always be greater or equal to bits_per_char

        static_assert(bits_per_char_unit >= bits_per_char);
        static_assert(sizeof(storage_unit) >= sizeof(letter_unit));

        /** @brief Returns the rank of the alphabet complement. */
        [[always_inline]] static constexpr storage_unit letter_complement(const storage_unit& letter) noexcept {
            return seqan3::complement(std::move((typename traits::alphabet_type()).assign_rank(letter))).to_rank();
        };

        /** @brief Constructs an alphabet symbol from a masked rank. */
        [[always_inline]] static constexpr alphabet_type letter_to_symbol(const storage_unit& letter) noexcept {
            return (alphabet_type()).assign_rank(letter & char_mask);
        };

        /** @brief Converts a masked rank to its alphabet character. */
        [[always_inline]] static constexpr char letter_to_char(const storage_unit& letter) noexcept {
            return letter_to_symbol(letter).to_char();
        };

        /**
         * @brief Builds the low-bit mask for a k-mer of a given length.
         * @tparam p_other_storage_unit Unsigned mask type.
         * @param length K-mer length in symbols.
         * @return Low-bit k-mer mask, saturated when the length exceeds the type.
         */
        template<typename p_other_storage_unit>
        [[always_inline]] static constexpr p_other_storage_unit kmer_mask(const size_t& length) noexcept {
            constexpr size_t max_length = sizeof(p_other_storage_unit) * 8 / bits_per_char;
            if (length > max_length) {
                return std::numeric_limits<p_other_storage_unit>::max(); //static_cast<p_other_storage_unit>(-1);
            }
            return (static_cast<p_other_storage_unit>(1) << (length * bits_per_char)) - 1;
        };


    };

    using derived_range_type = p_derived_range;

    /** @brief Marks iterators assignable from the derived range's begin type. */
    template<typename p_iterator_type>
    struct valid_derived_iterator {
        static constexpr bool value = false;
    };

    /** @brief Marks an iterator assignable from the derived range's begin type. */
    template <typename p_iterator_type>
        requires(requires(derived_range_type d, p_iterator_type i) {
            { i = d.begin() };
        })
    struct valid_derived_iterator<p_iterator_type> {
        static constexpr bool value = true;
    };

    /** @brief Returns the concrete range through the CRTP base. */
    inline constexpr derived_range_type const* super() const {
        return static_cast<derived_range_type const*>(this);
    };

    // forward bp iterator

    /** @brief Selects the dereference value for a packed iterator mode. */
    template<iterator_mode p_mode>
    struct packed_iterator_traits {
        using dereference_type = std::conditional_t<
            p_mode == logical_rank,
            typename traits::storage_unit,
            std::conditional_t<p_mode == alphabet_symbol, typename traits::alphabet_type, void>>;
    };

    /**
     * @brief Iterates logical ranks or symbols over LSB-first packed units.
     * @tparam p_base_iterator_type Random-access packed-unit iterator.
     * @tparam p_selected_mode Dereference representation.
     */
    template <typename p_base_iterator_type, iterator_mode p_selected_mode>
    class packed_sequence_iterator
        : public boost::iterator_adaptor<
              packed_sequence_iterator<p_base_iterator_type, p_selected_mode>,
              p_base_iterator_type,
              typename packed_iterator_traits<p_selected_mode>::dereference_type,
              boost::use_default,
              typename packed_iterator_traits<p_selected_mode>::dereference_type> {
      private:
        typedef boost::iterator_adaptor<
            packed_sequence_iterator<p_base_iterator_type, p_selected_mode>,
            p_base_iterator_type,
            typename packed_iterator_traits<p_selected_mode>::dereference_type,
            boost::use_default,
            typename packed_iterator_traits<p_selected_mode>::dereference_type>
            super_t;

        static constexpr iterator_mode selected_iterator_mode =
            p_selected_mode;

        std::size_t rank_slot_{};
        p_base_iterator_type end_{};
        typename traits::storage_unit current_unit_{};

    public:
        friend class boost::iterator_core_access;
        friend class compact_sequence_range_base;

        using iterator_type = p_base_iterator_type;

        packed_sequence_iterator() = default;

        /**
         * @brief Builds an iterator at a packed unit and rank slot.
         * @param it Current packed-unit iterator.
         * @param end One-past-last packed-unit iterator.
         * @param rank_slot Rank slot within the current unit.
         * @param current_unit Shifted current packed word.
         */
        explicit packed_sequence_iterator(
            p_base_iterator_type it,
            p_base_iterator_type end,
            const std::size_t rank_slot,
            typename traits::storage_unit current_unit
        ) : super_t(it), rank_slot_(rank_slot), end_(end),
            current_unit_(current_unit) {}

        /** @brief Returns the rank slot within the current packed unit. */
        constexpr std::size_t rank_slot() const noexcept {
            return rank_slot_;
        }

        /** @brief Orders iterators by packed unit and rank slot. */
        constexpr std::strong_ordering operator<=> (
            const packed_sequence_iterator& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return (this->base() <=> other.base());
            } else {
                if (auto cmp = this->base() <=> other.base(); cmp != 0) { return cmp; }
                return (rank_slot_ <=> other.rank_slot_);
            }
        }

        /** @brief Tests packed-unit and rank-slot identity. */
        constexpr bool operator==(
            const packed_sequence_iterator& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return (this->base() == other.base());
            } else {
                return this->base() == other.base()
                    && rank_slot_ == other.rank_slot_;
            }
        }

        /**
         * @brief Dereferences through a selected logical representation.
         * @tparam p_dereference_mode Rank or alphabet-symbol representation.
         */
        template <iterator_mode p_dereference_mode>
        typename packed_iterator_traits<p_dereference_mode>::dereference_type _dereference() const {
            if constexpr (p_dereference_mode == logical_rank) {
                if constexpr (traits::letters_per_unit == 1) {
                    return *this->base();
                } else {
                    return current_unit_ & traits::char_mask;
                }
            } else if constexpr (p_dereference_mode == alphabet_symbol) {
                typename traits::alphabet_type alph;
                return alph.assign_rank(_dereference<logical_rank>());
            } else {
                static_assert(
                    p_dereference_mode == logical_rank
                    || p_dereference_mode == alphabet_symbol,
                    "unsupported packed-sequence iterator mode");
            }
        }

        /** @brief Dereferences using the iterator's configured mode. */
        auto dereference() const {
            return _dereference<p_selected_mode>();
        }

        /** @brief Advances to the next logical symbol. */
        void increment() {
            if constexpr (traits::letters_per_unit == 1) {
                ++this->base_reference();
            } else if (rank_slot_ == traits::letters_per_unit - 1) {
                // Need to increment
                ++this->base_reference();
                if (this->base() != end_) {
                    current_unit_ = *this->base();
                }
                rank_slot_ = 0;
            } else {
                ++rank_slot_;
                current_unit_ >>= traits::bits_per_char;
            }
        }

        /** @brief Tests equality for Boost iterator adaptation. */
        bool equal(const packed_sequence_iterator& other) const {
            if constexpr (traits::letters_per_unit == 1) {
                return this->base() == other.base();
            } else {
                return this->base() == other.base()
                    && rank_slot_ == other.rank_slot_;
            }
        }
    };

    /** @brief Borrowed begin/end pair for one packed iterator mode. */
    template<typename p_iterator_type>
    struct packed_iterator_range {
    private:
        p_iterator_type begin_;
        p_iterator_type end_;

    public:
        /** @brief Binds a borrowed iterator pair. */
        packed_iterator_range(
            p_iterator_type begin_iterator,
            p_iterator_type end_iterator
        ): begin_(begin_iterator), end_(end_iterator) {}

        /** @brief Returns the first logical iterator. */
        p_iterator_type const& begin() const {
            return begin_;
        }

        /** @brief Returns the one-past-last logical iterator. */
        p_iterator_type const& end() const {
            return end_;
        }
    };

    /**
     * @brief Converts a packed-unit iterator to one logical iterator mode.
     * @tparam p_mode Rank or alphabet-symbol representation.
     * @tparam p_iterator_type Iterator accepted by the derived range.
     * @param iterator Packed-unit iterator.
     * @return Logical iterator preserving trailing padding semantics.
     */
    template<iterator_mode p_mode, typename p_iterator_type>
    requires (valid_derived_iterator<p_iterator_type>::value == true)
    constexpr inline packed_sequence_iterator<p_iterator_type, p_mode> iter(
        const p_iterator_type& iterator) const {
        if (super()->end() == iterator) {
            if constexpr (traits::letters_per_unit != 1) {
                if (super()->begin() != iterator && super()->padding() != 0) {
                    return packed_sequence_iterator<p_iterator_type, p_mode>(
                        iterator - 1,
                        super()->end(),
                        traits::letters_per_unit - super()->padding(),
                        typename traits::storage_unit{});
                }
            }
            return packed_sequence_iterator<p_iterator_type, p_mode>(
                iterator,
                super()->end(),
                0,
                typename traits::storage_unit{});
        }
        return packed_sequence_iterator<p_iterator_type, p_mode>(
            iterator, super()->end(), 0, *iterator);
    };

    /**
     * @brief Returns the complete logical iterator range for one mode.
     * @tparam p_mode Rank or alphabet-symbol representation.
     */
    template<iterator_mode p_mode>
    constexpr inline auto iterable() const {
        return packed_iterator_range(
            iter<p_mode>(super()->cbegin()),
            iter<p_mode>(super()->cend())
        );
    };

    /**
     * @brief Find an alphabet-symbol subsequence within a packed iterator range.
     * @tparam p_iterator_type Valid iterator type for the derived range.
     * @param begin First packed unit to inspect.
     * @param end One-past-last packed unit.
     * @param subsequence Alphabet symbols to locate.
     * @param start_position First logical-symbol position considered.
     * @return Logical-symbol offset of the first match, or `std::nullopt`.
     */
    /**
     * @brief Decodes a packed-unit interval to characters.
     * @tparam p_iterator_type Iterator accepted by the derived range.
     * @param begin First packed unit.
     * @param end One-past-last packed unit.
     * @param split_units Insert a separator before each packed unit.
     * @return Decoded characters in logical order.
     */
    template<typename p_iterator_type>
    requires (valid_derived_iterator<p_iterator_type>::value == true)
    constexpr std::optional<std::size_t> find_symbol_subsequence(
        p_iterator_type begin,
        p_iterator_type end,
        std::vector<typename traits::alphabet_type> subsequence,
        std::size_t start_position = 0) const
    {
        const auto sequence_string = to_string(begin, end);
        const auto subsequence_string = symbols_vector_to_string(subsequence);
        const auto position =
            sequence_string.find(subsequence_string, start_position);
        if (position == std::string::npos) {
            return std::nullopt;
        }
        return position;
    }

    template<typename p_iterator_type>
    requires (valid_derived_iterator<p_iterator_type>::value == true)
    constexpr std::string to_string(
        p_iterator_type begin,
        p_iterator_type end,
        bool split_units = false) const {
        std::string result;
        result.reserve((end - begin) * traits::letters_per_unit);
        const auto symbol_begin = iter<alphabet_symbol, p_iterator_type>(begin);
        const auto symbol_end = iter<alphabet_symbol, p_iterator_type>(end);
        for(auto letter = symbol_begin; letter != symbol_end; ++letter) {
            if (letter.rank_slot() == 0 && split_units) {
                result += "|";
            }
            result.push_back(letter->to_char());
        }
        return result;
    }

    /** @brief Decodes the complete range to alphabet characters. */
    constexpr std::string to_string(bool split_units = false) const {
        return to_string(super()->cbegin(), super()->cend(), split_units);
    }

    /** @brief Returns the logical symbol count excluding trailing padding. */
    constexpr size_t bp_size() const {
        return super()->size() * traits::letters_per_unit - super()->padding();
    }

    /** @brief Returns the packed storage byte count. */
    constexpr size_t byte_size() const {
        return super()->size() * sizeof(typename traits::storage_unit);
    }

    /**
     * @brief Split packed storage into unit-aligned record descriptors.
     *
     * No ranks are decoded or copied. Only the final descriptor carries the
     * range's trailing padding.
     *
     * @param record_bp Maximum logical symbols per record. Must be a multiple
     * of `traits::letters_per_unit`.
     * @return Tuples of unit offset, unit count, and trailing padding.
     * @throws std::invalid_argument If `record_bp` is zero or not unit-aligned.
     */
    [[nodiscard]] std::vector<std::tuple<std::size_t, std::size_t, std::size_t>>
    split_units(std::size_t record_bp) const {
        if (record_bp == 0
            || record_bp % traits::letters_per_unit != 0) {
            throw std::invalid_argument(
                "split_units requires a non-zero unit-aligned record size");
        }
        const std::size_t units_per_record = record_bp / traits::letters_per_unit;
        const std::size_t total_units = super()->size();
        std::vector<std::tuple<std::size_t, std::size_t, std::size_t>> out;
        if (total_units == 0) { out.emplace_back(0u, 0u, 0u); return out; }
        for (std::size_t off = 0; off < total_units; off += units_per_record) {
            const std::size_t count   = std::min(units_per_record, total_units - off);
            const bool        is_last = (off + count >= total_units);
            out.emplace_back(off, count, is_last ? super()->padding() : std::size_t{0});
        }
        return out;
    }

    /** @brief Advance a packed iterator by a number of storage units. */
    template<typename p_iterator_type>
    [[always_inline]] static inline void advance_iter(
        p_iterator_type& iterator,
        const size_t distance) {
        iterator += distance;
    }

    /** @brief Return the distance between iterators in packed storage units. */
    template<typename p_iterator_type>
    [[always_inline]] static inline size_t view_distance(
        const p_iterator_type& begin,
        const p_iterator_type& end) {
        return static_cast<size_t>(end - begin);
    }

    /** @brief Returns the number of distinct symbols in the alphabet. */
    static constexpr size_t alphabet_cardinality() {
        return seqan3::alphabet_size<p_alphabet>;
    }

    /**
     * @brief Extract all base-pair rank values to a vector of uint8_t.
     *
     * Each element in the returned vector is the rank value (0..alphabet_size-1)
     * of one nucleotide in left-to-right order. This is the format required by
     * algorithms like KSW2 that operate on raw rank arrays.
     *
     * For compact_sequence_range<std::vector<seq::dna4>>, the ranks are:
     *   A=0, C=1, G=2, T=3 (matching seqan3::to_rank).
     *
     * @return Vector of uint8_t rank values, one per nucleotide
     */
    [[nodiscard]] std::vector<uint8_t> to_encoded_vector() const {
        const size_t sequence_length = bp_size();
        // iterable<logical_rank>() constructs both endpoint iterators through
        // iter(),
        // whose representation dereferences the underlying iterator.
        // Avoid forming those iterators for an empty packed range.
        if (sequence_length == 0u) return {};
        std::vector<uint8_t> encoded_ranks;
        encoded_ranks.reserve(sequence_length);
        for (const auto& base_pair_rank : iterable<logical_rank>()) {
            encoded_ranks.push_back(static_cast<uint8_t>(base_pair_rank));
        }
        return encoded_ranks;
    }

    /**
     * @brief Extract a sub-region of the sequence as rank values, with optional
     *        reverse-complement transformation.
     *
     * @tparam p_is_reversed  When false, encodes [bp_from, bp_to) in forward order.
     *                      When true, produces the same result as slicing the full
     *                      reverse-complement encoding at [bp_from, bp_to).
     * @param bp_from  Start position (inclusive, 0-based)
     * @param bp_to    End position (exclusive, 0-based)
     * @return Vector of uint8_t rank values for the requested region
     */
    template<bool p_is_reversed>
    [[nodiscard]] std::vector<uint8_t> to_encoded_vector(size_t bp_from, size_t bp_to) const {
        if (bp_from >= bp_to) return {};
        const size_t region_length = bp_to - bp_from;
        std::vector<uint8_t> encoded_ranks(region_length);

        if constexpr (!p_is_reversed) {
            size_t write_pos = 0;
            size_t pos = 0;
            for (const auto& bp_rank : iterable<logical_rank>()) {
                if (pos >= bp_to) break;
                if (pos >= bp_from) {
                    encoded_ranks[write_pos++] = static_cast<uint8_t>(bp_rank);
                }
                ++pos;
            }
        } else {
            // Reverse-complement maps each output rank to its mirrored input.
            // read forward, complemented, written in reverse order.
            const size_t sequence_length = bp_size();
            const size_t orig_from = sequence_length - bp_to;
            const size_t orig_to = sequence_length - bp_from;
            size_t write_pos = region_length;
            size_t pos = 0;
            for (const auto& bp_rank : iterable<logical_rank>()) {
                if (pos >= orig_to) break;
                if (pos >= orig_from) {
                    --write_pos;
                    encoded_ranks[write_pos] = static_cast<uint8_t>(
                        traits::letter_complement(
                            static_cast<p_packed_unit>(bp_rank)));
                }
                ++pos;
            }
        }
        return encoded_ranks;
    }

    /**
     * @brief Encode a sub-region directly into caller-provided storage.
     *
     * Same output as `to_encoded_vector<p_is_reversed>(bp_from, bp_to)` but
     * writes into a span owned by the caller. Used by KSW2 to skip the
     * per-call std::vector allocation. The caller allocates
     * the destination from its own align arena and passes the span in.
     *
     * The destination span must be at least `bp_to - bp_from` bytes.
     *
     * Iteration seeks directly to `bp_from`, so runtime and memory traffic are
     * proportional to the selected region rather than the complete sequence.
     * The current implementation seeks directly to `bp_from` via index
     * arithmetic on the bit-packed storage, then walks exactly
     * `region_length` letters.
     */
    template<bool p_is_reversed, bool p_write_reversed = false>
    void to_encoded_into(std::span<uint8_t> dst, size_t bp_from, size_t bp_to) const {
        if (bp_from >= bp_to) return;
        const size_t region_length = bp_to - bp_from;
        if (dst.size() < region_length) return;

        constexpr size_t lpu = traits::letters_per_unit;
        constexpr size_t bpc = traits::bits_per_char;

        // The output of this method is the cross product of two flips:
        //   * letter complement (p_is_reversed)
        //   * write-pointer direction (the canonical rc has both flipped)
        //
        // To make the matrix legible:
        //
        //   <false, false>  forward     dst[i] = src[from+i]
        //   <false, true>   reverse     dst[i] = src[to-1-i]              (no rc, just mirror)
        //   <true,  false>  rc          dst[i] = complement(src[to-1-i])  (canonical rc)
        //   <true,  true>   complement  dst[i] = complement(src[from+i])  (rc twice cancels)
        //
        // The KSW2 / Block aligner pipelines pre-reverse the query before the
        // DP loop. When the alignment region is on the reverse strand
        // (p_is_reversed == true) the existing code path materialised a
        // reverse-complement buffer and then mirror-copied it into the DP
        // state — a wasteful round trip. With this template parameter we can
        // emit the alignment-ready orientation directly: the DP query buffer
        // wants the *complement only* form (rc twice = identity on order),
        // which is exactly <true, true> below.
        //
        // Translate to forward-domain coordinates. p_is_reversed reads through
        // the mirrored window of the source so that "complement + mirror"
        // produces the canonical rc; p_write_reversed only changes the
        // destination write order, not the source coordinates.
        const size_t fwd_from = p_is_reversed ? (bp_size() - bp_to)   : bp_from;
        [[maybe_unused]] const size_t fwd_to =
            p_is_reversed ? (bp_size() - bp_from) : bp_to;

        // The four orientations collapse to two output orderings in this
        // implementation: <true, false> and <false, true> both write
        // descending; <false, false> and <true, true> both write ascending.
        // Letter complement is applied iff p_is_reversed (mirroring how the
        // forward-window is interpreted). Branch is constexpr so the
        // generated code paths stay specialised per call site.
        constexpr bool descending_write = (p_is_reversed && !p_write_reversed)
            || (!p_is_reversed && p_write_reversed);

        // -----------------------------------------------------------
        // Fast path: alphabet packs exactly into storage_unit
        // (`unit_leftover_bits == 0`) and the requested window is at
        // least one full unit. Decompose each storage_unit via the
        // constexpr `extract_ranks_from_unit<>` helper and stream
        // into the destination buffer with no per-letter shift loop.
        //
        // For dna4 / sequence_custom_alphabet<dna4, uint64_t> this is
        // 32 letters per uint64_t, so a 1 Mbp KSW2 region encode
        // collapses ~62k unit reads + 62k inlined decompositions
        // into a tight loop the compiler can keep in registers and
        // vectorize against the destination span.
        //
        // The fallback below handles dna5 (5 letters → 3 bits has 2
        // leftover bits per uint8) and any future alphabet that
        // doesn't pack cleanly. Both code paths are gated by
        // `if constexpr` so each instantiation gets exactly one body.
        // -----------------------------------------------------------
        constexpr bool unit_aligned = (traits::unit_leftover_bits == 0);

        auto base_iter = super()->cbegin();
        std::advance(base_iter, fwd_from / lpu);
        size_t bit_no = fwd_from % lpu;

        if constexpr (unit_aligned) {
            // The fast path requires the source iterator to be
            // unit-aligned at the start. If `bp_from` lands mid-unit
            // we walk a few letters scalar-style until we reach the
            // next unit boundary, then switch to the unit-decomposed
            // bulk loop. Same idea on the tail.
            size_t produced = 0;

            auto emit_letter = [&dst, &region_length, &produced](
                                   typename traits::storage_unit raw) {
                const auto letter = p_is_reversed
                    ? static_cast<uint8_t>(traits::letter_complement(raw))
                    : static_cast<uint8_t>(raw);
                if constexpr (descending_write) {
                    dst[region_length - 1 - produced] = letter;
                } else {
                    dst[produced] = letter;
                }
                ++produced;
            };

            // Walk to the next unit boundary, scalar-shift style.
            // bioseq encodes the FIRST input letter at the unit's LSB
            // slot (see `extract_ranks_from_unit` header comment).
            // To advance past `bit_no` earlier letters we drop
            // the bottom `bit_no * bpc` bits and continue masking the
            // LSB — the same invariant held by packed_sequence_iterator.
            if (bit_no != 0 && produced < region_length) {
                typename traits::storage_unit unit_val =
                    static_cast<typename traits::storage_unit>(*base_iter);
                unit_val = static_cast<typename traits::storage_unit>(
                    unit_val >> (bit_no * bpc));
                while (bit_no < lpu && produced < region_length) {
                    emit_letter(static_cast<typename traits::storage_unit>(
                        unit_val & traits::char_mask));
                    unit_val = static_cast<typename traits::storage_unit>(
                        unit_val >> bpc);
                    ++bit_no;
                }
                // Only advance to the next unit if we consumed the
                // current one. Otherwise (region ended mid-unit) stay
                // put — the bulk loop has bulk_units==0 in that case
                // and the tail loop also has produced==region_length,
                // so no dereference happens; we keep the iterator
                // valid as a defensive measure for future refactors.
                if (bit_no == lpu) {
                    ++base_iter;
                    bit_no = 0;
                }
            }

            // Bulk loop: each iteration consumes one entire unit and
            // emits `lpu` letters. The decomposition is fully
            // constexpr — the compiler unrolls into a fixed pattern
            // of mask + shift + store instructions per unit.
            const size_t bulk_units = (region_length - produced) / lpu;
            for (size_t u = 0; u < bulk_units; ++u) {
                const auto ranks = extract_ranks_from_unit<
                    typename traits::storage_unit, bpc, lpu>(
                        static_cast<typename traits::storage_unit>(*base_iter));
                // Index 0 of `ranks` is the left-most symbol of the
                // unit — this matches the canonical sequence order.
                for (size_t k = 0; k < lpu; ++k) {
                    emit_letter(ranks[k]);
                }
                ++base_iter;
            }

            // Tail: a final partial unit, if region_length wasn't a
            // multiple of lpu after the alignment walk above.
            if (produced < region_length) {
                typename traits::storage_unit unit_val =
                    static_cast<typename traits::storage_unit>(*base_iter);
                while (produced < region_length) {
                    emit_letter(static_cast<typename traits::storage_unit>(
                        unit_val & traits::char_mask));
                    unit_val = static_cast<typename traits::storage_unit>(
                        unit_val >> bpc);
                }
            }
            return;
        }

        // -----------------------------------------------------------
        // Generic path (alphabets with leftover bits, e.g. dna5 in
        // uint8). Walks letter-by-letter via a scalar shift loop —
        // same logical-rank and padding contract as the aligned path.
        // -----------------------------------------------------------
        // For the letters_per_unit == 1 case, every unit holds exactly one
        // letter; iter<logical_rank>() is a thin wrapper around
        // dereferencing the underlying storage. For multi-letter units,
        // pre-shift the unit so the bp_from-th letter sits in the bottom
        // char_mask bits — the packed iterator invariant.
        typename traits::storage_unit unit_val{};
        if constexpr (lpu == 1) {
            unit_val = static_cast<typename traits::storage_unit>(*base_iter);
        } else {
            unit_val = static_cast<typename traits::storage_unit>(*base_iter);
            unit_val = static_cast<typename traits::storage_unit>(
                unit_val >> (bit_no * bpc));
        }

        auto next_letter = [&unit_val, &base_iter, &bit_no]() -> typename traits::storage_unit {
            const typename traits::storage_unit out =
                static_cast<typename traits::storage_unit>(unit_val & traits::char_mask);
            if constexpr (lpu == 1) {
                ++base_iter;
                // Defer dereference to the next iteration's read; only
                // refresh `unit_val` if we're going to read it.
                unit_val = 0;
            } else {
                ++bit_no;
                if (bit_no == lpu) {
                    bit_no = 0;
                    ++base_iter;
                    unit_val = static_cast<typename traits::storage_unit>(0);
                } else {
                    unit_val = static_cast<typename traits::storage_unit>(
                        unit_val >> bpc);
                }
            }
            return out;
        };

        // Refresh the unit at the *start* of each new storage word; the
        // letters_per_unit == 1 path needs this on every step, the multi
        // path only when bit_no wraps to 0.
        auto refresh_unit_if_needed =
            [&unit_val, &base_iter, &bit_no, &region_length](size_t step_idx) {
                if constexpr (lpu == 1) {
                    unit_val = static_cast<typename traits::storage_unit>(*base_iter);
                } else {
                    if (bit_no == 0 && step_idx + 1 < region_length) {
                        unit_val = static_cast<typename traits::storage_unit>(*base_iter);
                    }
                }
            };

        if constexpr (!descending_write) {
            for (size_t i = 0; i < region_length; ++i) {
                if constexpr (p_is_reversed) {
                    dst[i] = static_cast<uint8_t>(
                        traits::letter_complement(next_letter()));
                } else {
                    dst[i] = static_cast<uint8_t>(next_letter());
                }
                refresh_unit_if_needed(i);
            }
        } else {
            size_t write_pos = region_length;
            for (size_t i = 0; i < region_length; ++i) {
                --write_pos;
                if constexpr (p_is_reversed) {
                    dst[write_pos] = static_cast<uint8_t>(
                        traits::letter_complement(next_letter()));
                } else {
                    dst[write_pos] = static_cast<uint8_t>(next_letter());
                }
                refresh_unit_if_needed(i);
            }
        }
    }

    /**
     * @brief Extract reverse-complement base-pair rank values to a vector of uint8_t.
     *
     * Iterates the sequence left-to-right, complements each nucleotide using
     * the alphabet's complement operation (e.g. A<->T, C<->G for DNA), and
     * writes into the result vector in reverse order.
     *
     * Requires the alphabet to support seqan3::complement (nucleotide alphabets).
     *
     * @return Vector of uint8_t rank values representing the reverse complement
     */
    [[nodiscard]] std::vector<uint8_t> to_reverse_complement_encoded_vector() const {
        const size_t sequence_length = bp_size();
        std::vector<uint8_t> reverse_complement_ranks(sequence_length);
        size_t write_position = sequence_length;
        for (const auto& base_pair_rank : iterable<logical_rank>()) {
            --write_position;
            reverse_complement_ranks[write_position] = static_cast<uint8_t>(
                traits::letter_complement(
                    static_cast<p_packed_unit>(base_pair_rank)));
        }
        return reverse_complement_ranks;
    }

    /**
     * @brief Reinterprets the range through a compatible packed-unit width.
     * @tparam p_other_storage_unit Requested packed storage word type.
     * @return Borrowed bit-cast view preserving symbols and padding.
     */
    template<
        typename p_other_storage_unit,
        typename p_other_derived_type = compact_sequence_range<sequence_custom_alphabet<typename traits::alphabet_type, p_other_storage_unit, traits::bits_per_char>>
    >
    requires(
        is_sequence_custom_alphabet_bit_castable_to<
            sequence_custom_alphabet<typename traits::alphabet_type, typename traits::storage_unit, traits::bits_per_char>,
            sequence_custom_alphabet<typename traits::alphabet_type, p_other_storage_unit, traits::bits_per_char>
        >
    )
    [[always_inline]] constexpr bit_casted_view<
        p_derived_range, p_other_derived_type> bit_cast_to_storage_unit() const {
        return bit_casted_view<p_derived_range, p_other_derived_type>(*super());
    }

};

/**
 * @brief Borrows an unpacked alphabet range through the compact sequence API.
 * @tparam p_range_type Contiguous or iterable alphabet-symbol source.
 *
 * Each source element represents one logical symbol, so iteration and size
 * retain the source range's order without allocating packed storage.
 */
template<typename p_range_type>
requires (
    sequence_proper_container<p_range_type>
)
struct compact_sequence_range<p_range_type> : public compact_sequence_range_base<
    compact_sequence_range<p_range_type>,
    std::remove_reference_t<std::ranges::range_reference_t<p_range_type>>,
    typename boost::uint_t<seqan3::detail::ceil_log2(seqan3::alphabet_size<
        std::remove_reference_t<std::ranges::range_reference_t<p_range_type>>
    >)>::least,
    0
> {
public:
    using traits = typename compact_sequence_range<
        p_range_type>::compact_sequence_range_base::traits;
    using base_iterator_type = decltype(std::declval<p_range_type>().cbegin());

    const p_range_type& data_;

    /** @brief Returns zero because every source element stores one symbol. */
    static constexpr size_t padding() {
        return 0;
    }

    /** @brief Binds an rvalue range whose lifetime exceeds this adapter. */
    explicit constexpr compact_sequence_range(p_range_type&& base_range)
        : data_(std::move(base_range)) {}
    /** @brief Binds a borrowed alphabet-symbol range. */
    explicit constexpr compact_sequence_range(p_range_type& base_range)
        : data_(base_range) {}

    /** @brief Rank-projecting iterator over an unpacked alphabet range. */
    class unpacked_rank_iterator : public boost::iterator_adaptor<
        unpacked_rank_iterator,
        base_iterator_type,
        typename traits::storage_unit,
        boost::use_default,
        typename traits::storage_unit
    > {
    private:
        typedef boost::iterator_adaptor<
            unpacked_rank_iterator,
            base_iterator_type,
            typename traits::storage_unit,
            boost::use_default,
            typename traits::storage_unit
        > super_t;
    public:
        friend class boost::iterator_core_access;
        friend class compact_sequence_range<p_range_type>;

        template <typename p_friend_range_type>
        friend struct compact_sequence_range;

        unpacked_rank_iterator() = default;

        /** @brief Binds a source alphabet iterator. */
        explicit unpacked_rank_iterator(base_iterator_type const& it)
            : super_t(it) {}

        /** @brief Orders iterators by their source positions. */
        auto operator <=>(const unpacked_rank_iterator&) const = default;

        /** @brief Returns the integer rank of the current alphabet symbol. */
        traits::storage_unit dereference() const {
            return static_cast<traits::storage_unit>(seqan3::to_rank(*this->base()));
        };

        /** @brief Returns the source-element distance from another iterator. */
        size_t operator-(unpacked_rank_iterator const& other) const {
            return static_cast<size_t>(this->base() - other.base());
        };
    };

    using iterator = unpacked_rank_iterator;

    /** @brief Returns a mutable adapter iterator to the first symbol. */
    unpacked_rank_iterator begin() {
        return unpacked_rank_iterator(data_.cbegin());
    }

    /** @brief Returns a mutable adapter iterator past the final symbol. */
    unpacked_rank_iterator end() {
        return unpacked_rank_iterator(data_.cend());
    }

    /** @brief Returns a constant adapter iterator to the first symbol. */
    unpacked_rank_iterator begin() const {
        return unpacked_rank_iterator(data_.cbegin());
    }

    /** @brief Returns a constant adapter iterator past the final symbol. */
    unpacked_rank_iterator end() const {
        return unpacked_rank_iterator(data_.cend());
    }

    /** @brief Returns a constant adapter iterator to the first symbol. */
    unpacked_rank_iterator cbegin() const {
        return unpacked_rank_iterator(data_.cbegin());
    }

    /** @brief Returns a constant adapter iterator past the final symbol. */
    unpacked_rank_iterator cend() const {
        return unpacked_rank_iterator(data_.cend());
    }

    /** @brief Orders adapters by their borrowed ranges. */
    auto operator <=>(const compact_sequence_range<p_range_type>&) const = default;

    /** @brief Returns the rank at a logical symbol index. */
    const traits::storage_unit operator[](size_t index) const {
        return static_cast<traits::storage_unit>(seqan3::to_rank(data_[index]));
    }

    /** @brief Returns the number of logical symbols. */
    constexpr size_t size() const {
        return data_.size();
    }

    /** @brief Tests equality of the borrowed alphabet ranges. */
    friend inline constexpr bool operator==(
        const compact_sequence_range<p_range_type>& a,
        const compact_sequence_range<p_range_type>& b
    ) {
        return a.data_ == b.data_;
    }

    /** @brief Returns a rank-sized pointer to the current source element. */
    constexpr static const typename traits::storage_unit* read_cptr(
        const unpacked_rank_iterator& it) {
        static_assert(sizeof(typename traits::alphabet_type)
                      == sizeof(typename traits::storage_unit));
        static_assert(
            std::is_standard_layout_v<typename traits::alphabet_type>);

        return reinterpret_cast<const typename traits::storage_unit*>(
            std::addressof(*it.base()));
    };
};

/**
 * @brief Owns alphabet ranks packed LSB-first under one configuration.
 * @tparam p_configuration Alphabet, storage word, and rank-width configuration.
 */
template<typename p_configuration>
requires (is_sequence_custom_alphabet<p_configuration>)
struct compact_sequence_range<p_configuration> : compact_sequence_range_base<
    compact_sequence_range<p_configuration>,
    typename p_configuration::alphabet_type,
    typename p_configuration::storage_unit,
    p_configuration::letter_bit_width
> {
public:
    using traits = typename compact_sequence_range<
        p_configuration>::compact_sequence_range_base::traits;

    using base_container = std::vector<typename traits::storage_unit>;
    using iterator = std::vector<typename traits::storage_unit>::const_iterator;


    /** @brief LSB-first packed alphabet ranks. */
    base_container packed_units;

    /** @brief Unused rank slots in the final packed unit. */
    std::size_t padding_letters{};

    /** @brief Construct an empty packed sequence. */
    compact_sequence_range() = default;

    /**
     * @brief Pack a sized alphabet range into LSB-first storage units.
     * @param input_sequence Symbols to pack in iteration order.
     * @return Packed units and the number of unused final-unit slots.
     */
    template <typename p_input_range>
    requires (
        std::ranges::sized_range<p_input_range>
        && std::is_same_v<std::remove_reference_t<
            std::ranges::range_reference_t<p_input_range>>,
            typename p_configuration::alphabet_type>
    )
    static constexpr std::pair<base_container, std::size_t> pack_symbols(
        p_input_range&& input_sequence)
    {
        const std::size_t required_units =
            input_sequence.size() / traits::letters_per_unit
            + (input_sequence.size() % traits::letters_per_unit != 0);
        base_container units;
        units.reserve(required_units);
        typename traits::storage_unit partial_word{};
        std::size_t partial_letters = 0u;
        std::size_t emitted_symbols = 0u;
        auto write_unit = [&units](typename traits::storage_unit unit) {
            units.push_back(unit);
            return true;
        };
        for(const auto& letter : input_sequence) {
            const bool appended =
                bioseq::sequences::rank_pack_detail::append_valid_rank_lsb<
                    typename traits::storage_unit,
                    traits::bits_per_char,
                    traits::letters_per_unit>(
                        partial_word,
                        partial_letters,
                        emitted_symbols,
                        letter.to_rank(),
                        write_unit);
            DEBUG_ASSERT(appended,
                         "growing rank pack unexpectedly overflowed");
        }
        const auto finished =
            bioseq::sequences::rank_pack_detail::finish_lsb<
                typename traits::storage_unit,
                traits::bits_per_char,
                traits::letters_per_unit>(
                    partial_word, partial_letters, write_unit);
        DEBUG_ASSERT(finished.capacity_ok,
                     "growing rank pack unexpectedly overflowed");
        DEBUG_ASSERT(emitted_symbols == input_sequence.size(),
                     "rank pack emitted-symbol mismatch");
        DEBUG_ASSERT(units.size() == required_units,
                     "rank pack unit-count mismatch");
        return std::make_pair(
            std::move(units),
            static_cast<std::size_t>(finished.padding_letters));
    }

    template <typename p_input_range>
    requires (
        std::ranges::sized_range<p_input_range>
        && std::is_same_v<std::remove_reference_t<
            std::ranges::range_reference_t<p_input_range>>,
            typename p_configuration::alphabet_type>
    )
    /**
     * @brief Construct a packed sequence from a sized alphabet range.
     * @param input_sequence Symbols to pack in iteration order.
     */
    compact_sequence_range(p_input_range&& input_sequence) {
        auto [units, padding] = pack_symbols(
            std::forward<p_input_range>(input_sequence));
        packed_units = std::move(units);
        padding_letters = padding;
    }

    /**
     * @brief Adopt already-packed LSB-first storage without re-encoding.
     * @param units Packed units whose first symbol occupies the low bits.
     * @param padding Unused symbol slots in the final unit.
     * @return Compact sequence owning the supplied storage.
     */
    [[nodiscard]] static constexpr compact_sequence_range from_packed(
        base_container&& units,
        std::size_t padding) noexcept {
        DEBUG_ASSERT(padding < traits::letters_per_unit,
                     "from_packed: padding must be < letters_per_unit");
        compact_sequence_range result;
        result.packed_units = std::move(units);
        result.padding_letters = padding;
        return result;
    }

    /** @brief Returns unused rank slots in the final packed unit. */
    constexpr std::size_t padding() const {
        return padding_letters;
    }

    /** @brief Returns an iterator to the first packed unit. */
    auto constexpr begin() { return packed_units.cbegin(); };
    /** @brief Returns an iterator past the final packed unit. */
    auto constexpr end() { return packed_units.cend(); };
    /** @brief Returns a constant iterator to the first packed unit. */
    auto constexpr begin() const { return packed_units.cbegin(); };
    /** @brief Returns a constant iterator past the final packed unit. */
    auto constexpr end() const { return packed_units.cend(); };
    /** @brief Returns a constant iterator to the first packed unit. */
    auto constexpr cbegin() const { return packed_units.cbegin(); };
    /** @brief Returns a constant iterator past the final packed unit. */
    auto constexpr cend() const { return packed_units.cend(); };

    /** @brief Returns the number of packed storage units. */
    auto constexpr size() const { return packed_units.size(); }

    /** @brief Orders ranges by packed units and padding. */
    auto operator <=> (
        const compact_sequence_range<p_configuration>&) const = default;
    /** @brief Tests packed units and padding for equality. */
    bool constexpr operator==(
        const compact_sequence_range<p_configuration>&) const = default;

    /** @brief Returns the address of the packed unit at an iterator. */
    constexpr static const typename traits::storage_unit* read_cptr(
        const iterator& it) {
        return std::to_address(it);
    };
};

/**
 * @brief Uses the default storage type and rank width for one alphabet.
 * @tparam p_alphabet_type Logical sequence alphabet.
 */
template<sequence_alphabet p_alphabet_type>
struct compact_sequence_range<p_alphabet_type>
    : compact_sequence_range<sequence_custom_alphabet<p_alphabet_type>> {
public:
    using parent = compact_sequence_range<
        sequence_custom_alphabet<p_alphabet_type>>;
    using traits = compact_sequence_range<p_alphabet_type>::parent::traits;
    using base_container =
        compact_sequence_range<p_alphabet_type>::parent::base_container;
    using iterator = compact_sequence_range<p_alphabet_type>::parent::iterator;
};

};
