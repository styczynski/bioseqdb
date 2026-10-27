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
 * @brief Adds symbol iteration and conversion methods to a packed range.
 * @tparam p_derived_range Range supplying packed-unit iterators.
 * @tparam p_alphabet Sequence alphabet.
 * @tparam p_packed_unit Packed word type.
 * @tparam p_bits_per_character Bits per ranj, or zero for  one rank per word.
 */
template<typename p_derived_range,
         typename p_alphabet,
         typename p_packed_unit,
         std::size_t p_bits_per_character>
struct compact_sequence_range_base {

    using self_type = compact_sequence_range_base<
        p_derived_range, p_alphabet, p_packed_unit, p_bits_per_character>;

    /** Alphabet settings and the number of symbols stored in each word. */
    struct traits {
        using alphabet_type = p_alphabet;
        using storage_unit = p_packed_unit;
        static constexpr bool is_bit_packed = true;
        static constexpr bool is_run_encoded = false;

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
        // A full, word needs an all-ones mask Building it by shifting one past
        // the word width would be undefined for types that do not promote wider.
        static constexpr storage_unit used_storage_mask =
            (unit_leftover_bits == 0)
                ? static_cast<storage_unit>(~static_cast<storage_unit>(0))
                : static_cast<storage_unit>((static_cast<storage_unit>(1) << unit_used_bits) - 1);

        static constexpr size_t alphabet_size =
            seqan3::alphabet_size<p_alphabet>;
        static constexpr size_t alphabet_size_n = alphabet_size + 1;

        using letter_unit = typename boost::uint_t<seqan3::detail::ceil_log2(alphabet_size)>::least; // Smallest unsigned type that can hold one alphabet rank.
        static constexpr size_t bits_per_char_unit = sizeof(letter_unit) * 8; // Width of a separately stored rank, in bits.

        static_assert(bits_per_char_unit >= bits_per_char);
        static_assert(sizeof(storage_unit) >= sizeof(letter_unit));

        /** @brief Returns the rank of the alphabet complement. */
        [[always_inline]] static constexpr storage_unit letter_complement(const storage_unit& letter) noexcept
            requires requires(alphabet_type symbol) { seqan3::complement(std::move(symbol)); } {
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
         * @brief Builds a mask for the packed bits of a k-mer.
         * @tparam p_other_storage_unit Unsigned mask type.
         * @param length K-mer length in symbols.
         *
         * Lengths beyond the type's symbol capacity return its maximum value.
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

    /** Returns this object as the derived range  type. */
    inline constexpr derived_range_type const* super() const {
        return static_cast<derived_range_type const*>(this);
    };

    // Forward symbol iteration.

    /** Chooses integer ranks or alphabet objects as iterator values. */
    template<iterator_mode p_mode>
    struct packed_iterator_traits {
        using dereference_type = std::conditional_t<
            p_mode == logical_rank,
            typename traits::storage_unit,
            std::conditional_t<p_mode == alphabet_symbol, typename traits::alphabet_type, void>>;
    };

    /**
     * @brief Walks symbols within packed words, starting from the low bits.
     * @tparam p_base_iterator_type Iterator over packed words.
     * @tparam p_selected_mode Return ranks or alphabet objects.
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
         * @brief Starts at a symbol slot within a packed word.
         * @param it Current word iterator.
         * @param end Iterator past the last word.
         * @param rank_slot Symbol slot in the current word.
         * @param current_unit Current word shifted so the selected rank occupies the low bits.
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

        /** Checks whether two iterators select the same symbol position. */
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
         * @brief Reads the current symbol as a rank or alphabt object.
         * @tparam p_dereference_mode Requested result form.
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
                // Continue at the  first symbol of the next packed word.
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

    /** Begin and end iterators for a borrowed sequence of symbols. */
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
     * @brief Converts a packed-word iterator into a symbol iterator.
     *
     * For the range end, selects the first padding slot so symbol iteration stops
     * before trailing padding
     * @tparam p_mode Return ranks or alphabet objects.
     * @param iterator Iterator accepted by the derived range.
     * @return Symbol iterator.
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
     * @brief Returns symbol iterators over the complete range.
     * @tparam p_mode Return ranks or alphabet objects.
     */
    template<iterator_mode p_mode>
    constexpr inline auto iterable() const {
        return packed_iterator_range(
            iter<p_mode>(super()->cbegin()),
            iter<p_mode>(super()->cend())
        );
    };

    /**
     * @brief Finds a symbol subsequence in a packed-word interval.
     *
     * Decodes the interval and pattern to strings before searching.
     * @param begin First packed word.
     * @param end Iterator past the  last word.
     * @param subsequence Symbols to find.
     * @param start_position First symbol offset to search, relative to begin.
     * @return Offset of the first match within the interval, or, no value.
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

    /** Decodes the range to characters, optionally separating packed words with |. */
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
     * @brief Describes unit-aligned chunks of packed storage without copying symbols.
     *
     * Only the last chunk carries trailing padding. Empty input produces one empty
     * chunk descriptor.
     * @param record_bp Maximum symbols per chunk, a multiple of letters_per_unit.
     * @return Tuples of unit offset, unit count, and trailing padding.
     * @throws std::invalid_argument If record_bp is zero or not unit-aligned.
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
     * @brief Copies sequence ranks into one byte per symbol.
     *
     * Used by alignment code such as KSW2. DNA4 ranks are A=0, C=1, G=2, T=3.
     * The alphabet ranks yust fit in uint8_t.
     * @return Rank bytes in  sequence order.
     */
    [[nodiscard]] std::vector<uint8_t> to_encoded_vector() const {
        const size_t sequence_length = bp_size();
        // Empty input needs no rank iterators or output allocation.
        if (sequence_length == 0u) return {};
        std::vector<uint8_t> encoded_ranks;
        encoded_ranks.reserve(sequence_length);
        for (const auto& base_pair_rank : iterable<logical_rank>()) {
            encoded_ranks.push_back(static_cast<uint8_t>(base_pair_rank));
        }
        return encoded_ranks;
    }

    /**
     * @brief Copies an interval into one byte per rank.
     * @tparam p_is_reversed Select coordinates in the full reverse complement when true.
     * @param bp_from First symbol, inclusive, in the selected orientation.
     * @param bp_to Last symbol, exclusive, in the selected orientation.
     * @return Rank bytes, or an empty vector if bp_from >= bp_to.
     *
     * The interval must lie within the sequence, and ranks must fit in uint8_t.
     * Reverse-complement output requires an  alphabet with complementation
     */
    template<bool p_is_reversed>
        requires (!p_is_reversed || requires(typename traits::storage_unit rank) {
            traits::letter_complement(rank);
        })
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
            // Read the mirrored source interval, complement its ranks, and write them
            // in reverse order.
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
     * @brief Writes an interval as rank bytes into caller-owned storage.
     *
     * Alignment code can reuse its work buffer instead of allocating a vector for
     * each interval. Decoding starts at the requested packed word.
     * @tparam p_is_reversed Interpret coordinates in the full reverse complement.
     * @tparam p_write_reversed Reverse the selected output before writing it.
     * @param dst Destination  with at least bp_to - bp_from bytes.
     * @param bp_from First symbol, inclusive.
     * @param bp_to Last symbol, exclusive.
     *
     * The interval, mugt lie within the sequence and ranks must fit in uint8_t.
     * An empty interval or undersized destination leaves dst unchanged.
     */
    template<bool p_is_reversed, bool p_write_reversed = false>
        requires (!p_is_reversed || requires(typename traits::storage_unit rank) {
            traits::letter_complement(rank);
        })
    void to_encoded_into(std::span<uint8_t> dst, size_t bp_from, size_t bp_to) const {
        if (bp_from >= bp_to) return;
        const size_t region_length = bp_to - bp_from;
        if (dst.size() < region_length) return;

        constexpr size_t lpu = traits::letters_per_unit;
        constexpr size_t bpc = traits::bits_per_char;

        // Coordinates refer to the forward sequence or its full reverse complement,
        // according to p_is_reversed. Map the latter interval back to source words.
        // p_write_reversed changes output order without changing that source interval.
        // With  both flags true, alignment receives complemented ranks in source order.
        const size_t fwd_from = p_is_reversed ? (bp_size() - bp_to)   : bp_from;
        [[maybe_unused]] const size_t fwd_to =
            p_is_reversed ? (bp_size() - bp_from) : bp_to;

        // Reverse-complement output writes ranks backward. Reversing the requested
        // output again restores forward writing, while complementation stays enabled.
        constexpr bool descending_write = (p_is_reversed && !p_write_reversed)
            || (!p_is_reversed && p_write_reversed);

        // When symbols fill each word exactly, decode whole words between the
        // partial first and last words. DNA4 in uint64_t holds 32 symbols per word.
        // Alphabets with unused bits in each word use the scalar path below
        constexpr bool unit_aligned = (traits::unit_leftover_bits == 0);

        auto base_iter = super()->cbegin();
        std::advance(base_iter, fwd_from / lpu);
        size_t bit_no = fwd_from % lpu;

        if constexpr (unit_aligned) {
            // Decode the partial  first word before entering the whole-word loop.
            size_t produced = 0;

            auto emit_letter = [&dst, &region_length, &produced](
                                   typename traits::storage_unit raw) {
                auto letter = static_cast<uint8_t>(raw);
                if constexpr (p_is_reversed) {
                    letter = static_cast<uint8_t>(traits::letter_complement(raw));
                }
                if constexpr (descending_write) {
                    dst[region_length - 1 - produced] = letter;
                } else {
                    dst[produced] = letter;
                }
                ++produced;
            };

            // Discard earlier slots so the first requested rank occupies the low bits.
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
                // Advance only if the first partial word was consumed cmpletely.
                if (bit_no == lpu) {
                    ++base_iter;
                    bit_no = 0;
                }
            }

            // Decode complete words using the fixed number of symbols per word.
            const size_t bulk_units = (region_length - produced) / lpu;
            for (size_t u = 0; u < bulk_units; ++u) {
                const auto ranks = extract_ranks_from_unit<
                    typename traits::storage_unit, bpc, lpu>(
                        static_cast<typename traits::storage_unit>(*base_iter));
                // ranks[0] is the first sequence symbol in this word.
                for (size_t k = 0; k < lpu; ++k) {
                    emit_letter(ranks[k]);
                }
                ++base_iter;
            }

            // Decode any symbols left in the final partial word.
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

        // For layouts with unused bits in each word, extract ranks one at a time
        // and restart at the next packed word after its  last complete symbol.
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
                // The refresh helper loads the next word.
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

        // Refresh after, moving to a new word. Multi-symbol words are reloaded
        // only when more output remains.
        auto refresh_unit_if_needed =
            [&unit_val, &base_iter, &bit_no, &region_length](size_t step_idx) {
                if (step_idx + 1u >= region_length) return;
                if constexpr (lpu == 1) {
                    unit_val = static_cast<typename traits::storage_unit>(*base_iter);
                } else {
                    if (bit_no == 0) {
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
     * @brief Returns the reverse complement as one byte per rank.
     *
     * Requires alphabet complementation and ranks that fit in uint8_t.
     * @return Complemented ranks in reverse sequence order.
     */
    [[nodiscard]] std::vector<uint8_t> to_reverse_complement_encoded_vector() const
        requires requires(typename traits::storage_unit rank) {
            traits::letter_complement(rank);
        } {
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
     * @brief Borrows the same packed bytes through a different word width.
     * @tparam p_other_storage_unit Requested word type.
     * @return View with the same symbols and padding recalculated for the new width.
     *
     * The source range must outlive the  view.
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
 * @brief Borrows an unpacked alphabet container through the compact sequence API.
 *
 * Each element is one ymbol The adapter reads its rank without packing a copy.
 * @tparam p_range_type Sized random-access alphabet range that outlives the adapter.
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

    /** Borrows an rvalue range without extending its lifetime. Keep the range alive externally. */
    explicit constexpr compact_sequence_range(p_range_type&& base_range)
        : data_(std::move(base_range)) {}
    /** @brief Binds a borrowed alphabet-symbol range. */
    explicit constexpr compact_sequence_range(p_range_type& base_range)
        : data_(base_range) {}

    /** Reads integer ranks from an unpacked alphabet container. */
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

    /** Returns a read-only rank iterator to the first symbol. */
    unpacked_rank_iterator begin() {
        return unpacked_rank_iterator(data_.cbegin());
    }

    /** Returns a read-only rank iterator past the  last symbol. */
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

    /**
     * @brief Returns the current symbol's address cast to the rank-storage pointer type.
     *
     * Requires equal symbol and storage sizes and a standard-layout alphabet type.
     * The cast leaves the backing object's C++ type unchanged.
     */
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
 * @brief Owns packed alphabet ranks, with the first symbol in each word's low bits.
 * @tparam p_configuration Alphabet, word type, and bits per symbol.
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
     * @brief Packs a sized alphabet range into words in sequence order.
     * @param input_sequence Symbols to pack.
     * @return Packed words and unused symbol slots in the final word.
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
     * @brief Takes ownership, of already-packed words without encoding them  again.
     * @param units Words with the first symbol in each word's low bits.
     * @param padding Unused finae-word slots, less than letters_per_unit.
     * @return Sequence owning the supplied vector.
     *
     * An empty vector requires zero padding.
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


    auto operator <=> (
        const compact_sequence_range<p_configuration>&) const = default;

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
