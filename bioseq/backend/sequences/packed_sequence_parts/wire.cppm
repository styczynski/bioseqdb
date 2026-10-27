/**
 * @file wire.cppm
 * @brief Compact sequence headers, inline packed storage, and conversion callbacks.
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

export module bioseq.sequences:packed_sequence_wire;

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.common.templating;
import bioseq.common.checked_arithmetic;
import bioseq.sequences.rank_pack_core;
export import bioseq.sequences.compact_sequence_typmod;
import :packed_sequence_core;
import :packed_sequence_packing;
import :packed_sequence_range;
export import :packed_sequence_cigar;

export {

// Sequence storage defines the compact header and packed layout. PostgreSQL
// supplies allocation and input/output callbacks for that representation

namespace compact_sequence_stdc_detail {

/** Compute actual member padding before rounding the payload boundary to eight bytes. */
template<class VarlenaHeader, class AlphabetCode, class LogicalSize>
[[nodiscard]] consteval std::size_t reserved_deficit() {
    std::uint64_t offset = sizeof(VarlenaHeader);
    offset = *bioseq::common::checked_align_up(offset, alignof(AlphabetCode));
    offset += sizeof(AlphabetCode) + sizeof(std::uint8_t);
    offset = *bioseq::common::checked_align_up(offset, alignof(std::uint16_t));
    offset += sizeof(std::uint16_t);
    offset = *bioseq::common::checked_align_up(offset, alignof(LogicalSize));
    offset += sizeof(LogicalSize) + sizeof(bioseq::sequence_encoding) + sizeof(bioseq::cigar_space);
    return static_cast<std::size_t>(*bioseq::common::checked_align_up(offset, sizeof(std::uint64_t)) - offset);
}

/** Byte-aligned reserved storage cannot introduce additional implicit member padding. */
template<std::size_t Deficit>
struct alignment_reserved_holder {
    std::array<std::uint8_t, Deficit> reserved;
};
/** @brief Omits alignment storage when the fixed header is already aligned. */
template<>
struct alignment_reserved_holder<0> { };

}  // namespace compact_sequence_stdc_detail

/**
 * @brief Allocation callback returning storage for a compact sequence value.
 *
 * Accepts a byte count and returns a pointer convertible to p_allocated_type*.
 * The caller requires alignment suitable for the header and packed words. A null
 * pointer reports allocation failure.
 */
template<class p_allocator_type, class p_allocated_type>
concept stdc_allocator = requires (
    p_allocator_type&& allocator, std::size_t bytes) {
    { std::forward<p_allocator_type>(allocator)(bytes) }
        -> std::convertible_to<p_allocated_type*>;
};

/** @brief Identifies a supported standard-layout compact-sequence header. */
template<class p_candidate_type>
concept is_compact_sequence_stdc =
    requires {
        requires p_candidate_type::is_valid_compact_sequence_stdc == true;
        typename p_candidate_type::supported_alphabets_type;
        typename p_candidate_type::supported_storage_unit_byte_widths_type;
        typename p_candidate_type::bp_size_t;
        typename p_candidate_type::varlena_header_t;
        typename p_candidate_type::alphabet_code_t;
    }
    && supported_alphabet_set<
        typename p_candidate_type::supported_alphabets_type>
    && supported_storage_width_set<
        typename p_candidate_type::supported_storage_unit_byte_widths_type>;

/**
 * @brief Gives a compact header  a compile-time alphabet and word size.
 * @tparam p_stdc_type Compact hender type.
 * @tparam p_alphabet Supported alphabet.
 * @tparam p_storage_unit_byte_width Enabled word size in bytes.
 */
template<
    class p_stdc_type,
    sequence_alphabet p_alphabet,
    size_t p_storage_unit_byte_width
>
requires (
    is_compact_sequence_stdc<p_stdc_type>
    && alphabet_in_supported_set<
        typename p_stdc_type::supported_alphabets_type, p_alphabet>
    && p_stdc_type::supported_storage_unit_byte_widths_type::template contains<
        p_storage_unit_byte_width>
)
struct compact_sequence_stdc_instance;

/**
 * @brief Stores sequence metadata followed by packed words in the same allocation.
 *
 * The header records the alphabet and word size so runtime data can be passed
 * to a matching typed range. Payload alignment includes reserved header bytes.
 * @tparam p_alphabet_set_type Supported alphabets.
 * @tparam p_base_pair_size_type Integer, type for symbol counts.
 * @tparam p_varlena_header_type Integer type for the allocation-length field.
 * @tparam p_storage_width_set_type Allowed packed-word sizes.
 */
template<
    class p_alphabet_set_type,
    class p_base_pair_size_type = std::uint32_t,
    class p_varlena_header_type = std::int32_t,
    class p_storage_width_set_type =
        all_storage_unit_byte_widths
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<
        p_storage_width_set_type>
)
struct compact_sequence_stdc {
public:
    using supported_alphabets_type = p_alphabet_set_type;
    using supported_storage_unit_byte_widths_type =
        p_storage_width_set_type;
    using bp_size_t                = p_base_pair_size_type;
    using varlena_header_t         = p_varlena_header_type;
    using alphabet_code_t          = typename p_alphabet_set_type::alphabet_code_t;

    static constexpr bool is_valid_compact_sequence_stdc = true;

    static constexpr size_t alignment_deficit =
        compact_sequence_stdc_detail::reserved_deficit<
            varlena_header_t,
            alphabet_code_t,
            bp_size_t
        >();

    /** Allocation length encoded with PostgreSQL varlena format bits. */
    varlena_header_t      vl_len_;
    /** @brief Alphabet index in supported_alphabets_type. */
    alphabet_code_t       alphabet_code;
    /** @brief Width of one packed storage unit in bytes. */
    std::uint8_t          storage_unit_byte_width;
    /** @brief Unused alphabet slots in the final packed unit. */
    std::uint16_t         padding_letters;
    /** @brief Number of logical alphabet symbols. */
    bp_size_t             bp_size;
    /** Physical payload codec; does not introduce another alphabet dispatch axis. */
    bioseq::sequence_encoding encoding{bioseq::sequence_encoding::rank_packed};
    /** Declared CIGAR operation subset; ordinary alphabets use compatibility. */
    bioseq::cigar_space space{bioseq::cigar_space::compatibility};
    [[no_unique_address]]
    compact_sequence_stdc_detail::alignment_reserved_holder<alignment_deficit>
        alignment_padding;
    /** @brief First byte of the variable-length packed payload. */
    std::uint8_t          data[1];

    /** @brief Returns bytes from the allocation start to the packed payload. */
    [[nodiscard]] static constexpr size_t header_bytes() noexcept {
        return offsetof(compact_sequence_stdc, data);
    }

    /**
     * @brief Encodes the  allocation length above the two varlena format bits.
     * @param total_bytes Complete size, including the header, fitting in 30 bits.
     * @return Encoded length field
     */
    [[nodiscard]] static constexpr varlena_header_t
    encode_varlena_header(std::size_t total_bytes) noexcept {
        return static_cast<varlena_header_t>(
            static_cast<std::uint32_t>(total_bytes) << 2);
    }

    /**
     * @brief Calls a handler with the header type for a runtime alphabet and word size.
     * @param alpha_code Alphabet index in the supported set.
     * @param byte_width Enabled word size in bytes.
     * @param handler Callable with the matching instance type as a template argument.
     *
     * Invalid codes or widths fail an assertion.
     */
    template<class p_handler_type>
    [[gnu::always_inline]]
    static constexpr void dispatch_instance(
        alphabet_code_t alpha_code,
        std::uint8_t byte_width,
        p_handler_type&& handler)
    {
        p_alphabet_set_type::dispatch_alphabet_by_code(
            alpha_code, [&byte_width, &handler]<class p_dispatched_alphabet>() {
                if constexpr (bioseq::cigar_operation_alphabet<p_dispatched_alphabet>) {
                    constexpr auto run_width = sizeof(bioseq::cigar_run_value<p_dispatched_alphabet>);
                    static_assert(p_storage_width_set_type::template contains<run_width>,
                                  "CIGAR registration requires its native run record width");
                    DEBUG_ASSERT(byte_width == run_width, "CIGAR storage width must equal its run codec width");
                    handler.template operator()<compact_sequence_stdc_instance<
                        compact_sequence_stdc, p_dispatched_alphabet, run_width>>();
                } else {
                p_storage_width_set_type::dispatch(
                    byte_width, [&handler]<size_t p_dispatched_byte_width>() {
                        handler.template operator()<
                            compact_sequence_stdc_instance<
                                compact_sequence_stdc,
                                p_dispatched_alphabet,
                                p_dispatched_byte_width>
                        >();
                    });
                }
            });
    }

    /**
     * @brief Maps a runtime alphabet code to its display name.
     * @param code Runtime alphabet index.
     * @return Display name, or `"?"` for an invalid code.
     */
    [[nodiscard]] static std::string_view name_of_alphabet_code(
        alphabet_code_t code) noexcept
    {
        return p_alphabet_set_type::name_of_alphabet_code(code);
    }

    /**
     * @brief Allocates a compact value and reads its packed words through a callback.
     *
     * Checks that the supplied word count and padding agree with the symbol  count.
     * The callback supplies valid ranks in the selected packed layout.
     * @param target_storage_byte_width Incoming word size in bytes.
     * @param in_padding_letters Unused final-word symbol slots.
     * @param in_bp_size Sequence symbol count.
     * @param in_unit_count Packed-word count.
     * @param receive_context Context passed to receiver.
     * @param receiver Reads one ord as uint64_t for the supplied alphabet and width.
     * @param allocator Allocates the complete header and payload.
     * @return New value, or null when allocation fails.
     */
    template<
        sequence_alphabet p_alphabet_type,
        class p_receive_context_type,
        class p_receiver_type,
        class p_allocator_type
    >
    requires (
        alphabet_in_supported_set<p_alphabet_set_type, p_alphabet_type>
        && (!bioseq::cigar_operation_alphabet<p_alphabet_type>)
        && stdc_allocator<p_allocator_type, compact_sequence_stdc>
    )
    [[nodiscard]]
    static compact_sequence_stdc* convert_from(
        std::uint8_t target_storage_byte_width,
        std::uint16_t in_padding_letters,
        bp_size_t in_bp_size,
        std::uint32_t in_unit_count,
        p_receive_context_type& receive_context,
        p_receiver_type&& receiver,
        p_allocator_type&& allocator);

    /**
     * @brief Allocates a compact header with space for the requested packed words.
     *
     * Sets the logical sequence length to zero and leaves payload initialization
     * to the caller.
     * @param target_storage_byte_width Enabled word size in bytes.
     * @param unit_count_value Capacity in words.
     * @param allocator Allocates the header and rounded payload capacity.
     * @return Empty  value, or null when allocation fails.
     */
    template<
        sequence_alphabet p_alphabet_type,
        class p_allocator_type
    >
    requires (
        alphabet_in_supported_set<p_alphabet_set_type, p_alphabet_type>
        && (!bioseq::cigar_operation_alphabet<p_alphabet_type>)
        && stdc_allocator<p_allocator_type, compact_sequence_stdc>
    )
    [[nodiscard]]
    static compact_sequence_stdc* reserve(
        std::uint8_t target_storage_byte_width,
        std::uint32_t unit_count_value,
        p_allocator_type&& allocator);

    /**
     * @brief Sends every packed word through a callback in storage order.
     * @param send_context Context passed to sender.
     * @param sender Receives each word as uint64_t with its original byte width.
     */
    template<class p_send_context_type, class p_sender_type>
    void send_to(
        p_send_context_type& send_context,
        p_sender_type&& sender) const;

    /**
     * @brief Calls a handler with a typed range over this value's existing bytes.
     * @param handler Callable with the instance type and a temporary borrowed range
     *
     * The header and payload must outlive use of the range.
     */
    template<class p_handler_type>
    [[gnu::always_inline]]
    void convert_to(p_handler_type&& handler) const;

    /**
     * @brief Copies packed bytes into a value, with a different word width.
     *
     * Recalculates the word count and final padding. The caller must ensure the
     * layouts are compatible and the  source has enough readable payload bytes.
     * This function copies bytes without repacking individual ranks.
     * @param target_byte_width Enabled destination word size.
     * @param allocator Allocates the new value.
     * @return New vlue, or null when allocation fails.
     */
    template<class p_allocator_type>
    requires (stdc_allocator<p_allocator_type, compact_sequence_stdc>)
    [[nodiscard]]
    compact_sequence_stdc* recast_storage_width(
        std::uint8_t target_byte_width,
        p_allocator_type&& allocator) const;

    /** Returns the packed-word count for the stored alphabet and word size. */
    [[nodiscard]] size_t unit_count() const noexcept;

    /** @brief Returns payload bytes excluding trailing allocation alignment. */
    [[nodiscard]] size_t payload_bytes() const noexcept {
        if (encoding == bioseq::sequence_encoding::cigar_rle_fragment)
            return static_cast<std::size_t>(bp_size);
        if (encoding == bioseq::sequence_encoding::cigar_rle) {
            const auto total = static_cast<std::uint32_t>(vl_len_) >> 2u;
            return total >= header_bytes() ? total - header_bytes() : 0u;
        }
        return unit_count() * storage_unit_byte_width;
    }

    /** @brief Return the runtime alphabet and storage-width parameters. */
    [[nodiscard]] constexpr compact_sequence_stdc_type_params
    type_parameters() const noexcept {
        return {
            .alphabet_code           = static_cast<std::uint8_t>(alphabet_code),
            .storage_unit_byte_width = storage_unit_byte_width,
            .encoding = encoding,
            .space = space,
        };
    }
};

static_assert(
    std::is_standard_layout_v<
        compact_sequence_stdc<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>>>,
    "compact_sequence_stdc must be standard-layout for C-compatible storage");

namespace compact_sequence_stdc_detail {
/** @brief Checks payload alignment for one concrete header configuration. */
template<class p_alphabet_pack, class p_size_type, class p_header_type>
struct data_offset_check {
    using stdc_type = compact_sequence_stdc<p_alphabet_pack, p_size_type, p_header_type>;
    static constexpr bool aligned = (offsetof(stdc_type, data) % sizeof(std::uint64_t) == 0);
};

static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint32_t, std::int32_t>::aligned);
static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint64_t, std::int32_t>::aligned);
static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint16_t, std::int32_t>::aligned);
static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint8_t,  std::int32_t>::aligned);
static_assert(data_offset_check<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>,
                                std::uint32_t, std::int64_t>::aligned);
using cigar_header_check = compact_sequence_stdc<supported_alphabets<
    alphabet_entry<seq::dna4, "dna4">, alphabet_entry<bioseq::cigar_symbol, "cigar">>>;
static_assert(offsetof(cigar_header_check, data) == 16u,
              "CIGAR codec metadata must preserve the shared PostgreSQL payload offset");
}  // namespace compact_sequence_stdc_detail

/**
 * @brief Associates a compact header with one alphabet and packed-word type.
 * @tparam p_stdc_type Compact header type.
 * @tparam p_alphabet Supported alphabet.
 * @tparam p_storage_unit_byte_width Enabled word size in bytes.
 */
template<
    class p_stdc_type,
    sequence_alphabet p_alphabet,
    size_t p_storage_unit_byte_width
>
requires (
    is_compact_sequence_stdc<p_stdc_type>
    && alphabet_in_supported_set<
        typename p_stdc_type::supported_alphabets_type, p_alphabet>
    && p_stdc_type::supported_storage_unit_byte_widths_type::template contains<
        p_storage_unit_byte_width>
)
struct compact_sequence_stdc_instance {
    using compact_sequence_stdc_type = p_stdc_type;
    using alphabet_type              = p_alphabet;
    // Use standard fixed-width integer types so Highway selects the same lane
    // types across LP64 platforms. Boost  can choose a different C++ type with
    // the same width.
    /** Select a codec word without instantiating the other family's constrained type. */
    [[nodiscard]] static consteval auto storage_type_identity() {
        if constexpr (bioseq::cigar_operation_alphabet<p_alphabet>)
            return std::type_identity<bioseq::cigar_run_value<p_alphabet>>{};
        else if constexpr (p_storage_unit_byte_width == 8) return std::type_identity<std::uint64_t>{};
        else if constexpr (p_storage_unit_byte_width == 4) return std::type_identity<std::uint32_t>{};
        else if constexpr (p_storage_unit_byte_width == 2) return std::type_identity<std::uint16_t>{};
        else return std::type_identity<std::uint8_t>{};
    }
    using storage_unit = typename decltype(storage_type_identity())::type;
    static constexpr bool is_bit_packed = !bioseq::cigar_operation_alphabet<p_alphabet>;
    static constexpr bool is_run_encoded = !is_bit_packed;
    static_assert(is_bit_packed || p_storage_unit_byte_width == sizeof(storage_unit),
                  "run-encoded instances have exactly one physical record width");

    static constexpr size_t byte_width        = p_storage_unit_byte_width;
    static constexpr size_t letter_bit_width  =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);

    // Look up the alphabet name at compile time.
    static constexpr auto alphabet_name =
        p_stdc_type::supported_alphabets_type::template name_of<p_alphabet>();
    static constexpr std::string_view alphabet_name_sv = alphabet_name.view();

    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/**
 * @brief Borrows the packed payload of a compact value as a typed range.
 * @tparam p_instance_type Header type with a fixed alphabet and word size.
 */
template<typename p_instance_type>
requires (is_compact_sequence_stdc_instance<p_instance_type>
          && !bioseq::cigar_operation_alphabet<typename p_instance_type::alphabet_type>)
struct compact_sequence_range<p_instance_type>
    : public compact_sequence_range_base<
        compact_sequence_range<p_instance_type>,
        typename p_instance_type::alphabet_type,
        typename p_instance_type::storage_unit,
        p_instance_type::letter_bit_width
> {
public:
    using traits =
        typename compact_sequence_range<p_instance_type>::
            compact_sequence_range_base::traits;
    using base_container =
        const typename p_instance_type::compact_sequence_stdc_type*;
    // Iterator element type stays identical to the instance storage type.
    using iterator =
        const typename p_instance_type::storage_unit*;

private:
    const typename p_instance_type::compact_sequence_stdc_type& storage_;

public:
    /**
     * @brief Borrows a compact value whose alphabet and layout match this instance.
     * @param storage Header and payload that must outlive the view.
     *
     * Debug checks verify word width, symbol count, and final padding
     */
    explicit constexpr compact_sequence_range(
        const typename p_instance_type::compact_sequence_stdc_type& storage)
        noexcept
        : storage_(storage)
    {
        DEBUG_ASSERT(
            storage.storage_unit_byte_width == p_instance_type::byte_width,
            fmt::format("compact_sequence_range: storage width {} != "
                        "instance width {}",
                        static_cast<unsigned>(storage.storage_unit_byte_width),
                        static_cast<unsigned>(p_instance_type::byte_width)));
        DEBUG_ASSERT(
            storage.padding_letters < traits::letters_per_unit
            || (size() == 0 && storage.padding_letters == 0),
            fmt::format("padding_letters {} >= letters_per_unit {}",
                        static_cast<unsigned>(storage.padding_letters),
                        traits::letters_per_unit));
        DEBUG_ASSERT(
            static_cast<size_t>(storage.bp_size) + storage.padding_letters
            == size() * traits::letters_per_unit
            || (size() == 0 && storage.bp_size == 0
                && storage.padding_letters == 0),
            fmt::format("bp_size {} + padding {} != size {} * lpu {}",
                        static_cast<size_t>(storage.bp_size),
                        static_cast<unsigned>(storage.padding_letters),
                        size(), traits::letters_per_unit));
    }

    /** @return Number of unused logical alphabet slots in the final unit. */
    [[nodiscard]] constexpr size_t padding() const noexcept {
        return storage_.padding_letters;
    }

    /** @return Number of packed storage units occupied by the sequence. */
    [[nodiscard]] constexpr size_t size() const noexcept {
        const size_t bp = storage_.bp_size;
        if (bp == 0) return 0;
        return (bp + traits::letters_per_unit - 1) / traits::letters_per_unit;
    }

    /** @return Number of logical alphabet symbols in the sequence. */
    [[nodiscard]] constexpr size_t bp_size() const noexcept {
        return static_cast<size_t>(storage_.bp_size);
    }

    /** @brief Returns a constant iterator to the first packed unit. */
    [[nodiscard]] constexpr iterator cbegin() const noexcept {
        return reinterpret_cast<iterator>(storage_.data);
    }
    /** @brief Returns a constant iterator past the final packed unit. */
    [[nodiscard]] constexpr iterator cend() const noexcept {
        return cbegin() + size();
    }
    /** @brief Returns an iterator to the first packed unit. */
    [[nodiscard]] constexpr iterator begin() const noexcept { return cbegin(); }
    /** @brief Returns an iterator past the final packed unit. */
    [[nodiscard]] constexpr iterator end() const noexcept { return cend(); }

    /** @brief Returns a packed storage unit by index. */
    [[nodiscard]] constexpr typename traits::storage_unit operator[](
        size_t unit_index) const noexcept {
        return reinterpret_cast<const typename traits::storage_unit*>(
            storage_.data)[unit_index];
    }

    /** @brief Returns the packed-unit address carried by an iterator. */
    [[nodiscard]] constexpr static const typename traits::storage_unit*
    read_cptr(const iterator& it) noexcept {
        return it;
    }

    /** Compares sequence length and all payload bytes, including final-word padding bits. */
    [[nodiscard]] friend constexpr bool operator==(
        const compact_sequence_range<p_instance_type>& lhs,
        const compact_sequence_range<p_instance_type>& rhs)
    {
        if (&lhs.storage_ == &rhs.storage_) return true;
        if (lhs.storage_.bp_size != rhs.storage_.bp_size) return false;
        return std::memcmp(lhs.storage_.data, rhs.storage_.data,
            lhs.size() * sizeof(typename traits::storage_unit)) == 0;
    }
};

/** Borrow the canonical RLE payload of the same shared compact header. */
template<typename Instance>
    requires (is_compact_sequence_stdc_instance<Instance>
              && bioseq::cigar_operation_alphabet<typename Instance::alphabet_type>)
struct compact_sequence_range<Instance>
    : compact_sequence_range<typename Instance::alphabet_type> {
    using parent = compact_sequence_range<typename Instance::alphabet_type>;
    using traits = typename parent::traits;
    using iterator = typename parent::iterator;
    using base_container = const typename Instance::compact_sequence_stdc_type*;
    /** Borrow a validated complete value; internal fragment envelopes are never sequence views. */
    explicit compact_sequence_range(const typename Instance::compact_sequence_stdc_type& storage)
        : parent(std::span<const std::byte>{reinterpret_cast<const std::byte*>(storage.data), storage.payload_bytes()},
                 static_cast<std::uint32_t>(storage.bp_size), storage.space) {
        DEBUG_ASSERT(storage.encoding == bioseq::sequence_encoding::cigar_rle,
                     "only complete RLE values can become CIGAR sequence ranges");
        DEBUG_ASSERT(storage.storage_unit_byte_width == sizeof(typename traits::storage_unit)
                     && storage.padding_letters == 0u,
                     "CIGAR header does not match the canonical run codec");
    }
};

// Keep instance types separate  from custom alphabet configurations and
// alphabet containers so exactly one compact_sequence_range specialization
// matches. These assertions catch an overlap at its definition.
namespace compact_sequence_stdc_detail {
using checked_instance_type = compact_sequence_stdc_instance<
    compact_sequence_stdc<supported_alphabets<alphabet_entry<seq::dna4, "dna4">, alphabet_entry<seq::dna5, "dna5">>>,
    seq::dna4,
    size_t{8}>;
static_assert(!is_sequence_custom_alphabet<checked_instance_type>,
              "compact_sequence_stdc_instance must NOT satisfy "
              "is_sequence_custom_alphabet (partial-spec disambiguation)");
static_assert(!sequence_proper_container<checked_instance_type>,
              "compact_sequence_stdc_instance must NOT satisfy "
              "sequence_proper_container (partial-spec disambiguation)");
static_assert(is_compact_sequence_stdc_instance<checked_instance_type>);
}  // namespace compact_sequence_stdc_detail

};

// Out-of-line member definitions inherit export status from the class.

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<class p_handler_type>
[[gnu::always_inline]]
inline void compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::convert_to(
        p_handler_type&& handler) const
{
    dispatch_instance(
        alphabet_code, storage_unit_byte_width, [this, &handler]<class p_instance_type>() {
            handler.template operator()<p_instance_type>(
                compact_sequence_range<p_instance_type>{*this});
        });
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<class p_send_context_type, class p_sender_type>
inline void compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::send_to(
        p_send_context_type& send_context, p_sender_type&& sender) const
{
    dispatch_instance(
        alphabet_code,
        storage_unit_byte_width,
        [this, &send_context, &sender]<class p_instance_type>() {
            if constexpr (p_instance_type::is_run_encoded) {
                DEBUG_ASSERT(false, "CIGAR uses its family-specific canonical run wire frame");
            } else {
            using storage_unit_type = typename p_instance_type::storage_unit;
            constexpr std::size_t letters_per_unit =
                compact_sequence_range<p_instance_type>::traits::letters_per_unit;
            const std::size_t unit_count_value =
                (bp_size == 0) ? std::size_t{0}
                : (bp_size + letters_per_unit - 1) / letters_per_unit;
            const auto* reader =
                reinterpret_cast<const storage_unit_type*>(this->data);
            for (std::size_t unit_index = 0;
                 unit_index < unit_count_value;
                 ++unit_index) {
                sender.template operator()<std::uint64_t>(
                    send_context,
                    static_cast<std::uint64_t>(reader[unit_index]),
                    static_cast<std::uint8_t>(p_instance_type::byte_width));
            }
            }
        });
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
inline std::size_t compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::unit_count() const noexcept
{
    std::size_t result = 0;
    dispatch_instance(
        alphabet_code, storage_unit_byte_width, [this, &result]<class p_instance_type>() {
            if constexpr (p_instance_type::is_run_encoded) {
                if (encoding == bioseq::sequence_encoding::cigar_rle) {
                    const auto header = bioseq::read_cigar_payload_header(
                        std::span<const std::byte>{reinterpret_cast<const std::byte*>(data), payload_bytes()});
                    DEBUG_ASSERT(header.has_value(), "CIGAR payload prefix is truncated or invalid");
                    result = header ? header->run_count : 0u;
                } else {
                    DEBUG_ASSERT(false, "internal fragment envelopes have no logical run count");
                }
            } else {
            constexpr std::size_t letters_per_unit =
                compact_sequence_range<p_instance_type>::traits::letters_per_unit;
            result = (bp_size == 0) ? std::size_t{0}
                : (bp_size + letters_per_unit - 1) / letters_per_unit;
            }
        });
    return result;
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<sequence_alphabet p_alphabet, class p_allocator_type>
requires (
    alphabet_in_supported_set<p_alphabet_set_type, p_alphabet>
    && (!bioseq::cigar_operation_alphabet<p_alphabet>)
    && stdc_allocator<p_allocator_type, compact_sequence_stdc<
        p_alphabet_set_type,
        p_base_pair_size_type,
        p_varlena_header_type,
        p_storage_width_set_type>>
)
inline compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>*
compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::reserve(
    std::uint8_t target_storage_byte_width,
    std::uint32_t unit_count_value,
    p_allocator_type&& allocator)
{
    DEBUG_ASSERT(
        p_storage_width_set_type::contains_runtime(
            target_storage_byte_width),
        fmt::format("reserve: storage_unit_byte_width {} is not enabled",
                    static_cast<unsigned>(target_storage_byte_width)));

    const std::size_t payload_bytes_value =
        static_cast<std::size_t>(unit_count_value) * target_storage_byte_width;
    const std::size_t total_bytes =
        header_bytes() + round_up_to_uint64(payload_bytes_value);

    auto* header = std::forward<p_allocator_type>(allocator)(total_bytes);
    if (header == nullptr) return nullptr;
    header->vl_len_                 = encode_varlena_header(total_bytes);
    header->alphabet_code           =
        p_alphabet_set_type::template index_of<p_alphabet>();
    header->storage_unit_byte_width = target_storage_byte_width;
    header->padding_letters         = 0;
    header->bp_size                 = 0;
    header->encoding = bioseq::sequence_encoding::rank_packed;
    header->space = bioseq::cigar_space::compatibility;
    header->alignment_padding = {};
    return header;
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<
    sequence_alphabet p_alphabet,
    class p_receive_context_type,
    class p_receiver_type,
    class p_allocator_type
>
requires (
    alphabet_in_supported_set<p_alphabet_set_type, p_alphabet>
    && (!bioseq::cigar_operation_alphabet<p_alphabet>)
    && stdc_allocator<p_allocator_type, compact_sequence_stdc<
        p_alphabet_set_type,
        p_base_pair_size_type,
        p_varlena_header_type,
        p_storage_width_set_type>>
)
inline compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>*
compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::convert_from(
    std::uint8_t target_storage_byte_width,
    std::uint16_t in_padding_letters,
    p_base_pair_size_type in_bp_size,
    std::uint32_t in_unit_count,
    p_receive_context_type& receive_context,
    p_receiver_type&& receiver,
    p_allocator_type&& allocator)
{
    DEBUG_ASSERT(
        p_storage_width_set_type::contains_runtime(
            target_storage_byte_width),
        fmt::format(
            "convert_from(recv): storage_unit_byte_width {} is not enabled",
                    static_cast<unsigned>(target_storage_byte_width)));

    compact_sequence_stdc* result = nullptr;
    p_storage_width_set_type::dispatch(
        target_storage_byte_width,
        [&in_padding_letters,
         &in_bp_size,
         &in_unit_count,
         &receive_context,
         &receiver,
         &allocator,
         &result]<std::size_t p_storage_byte_width>() {
            using storage_unit_type =
                typename boost::uint_t<p_storage_byte_width * 8>::least;
            using sequence_configuration = sequence_custom_alphabet<
                p_alphabet,
                storage_unit_type,
                seqan3::detail::ceil_log2(
                    seqan3::alphabet_size<p_alphabet>)>;
            constexpr std::size_t letters_per_unit =
                compact_sequence_range<sequence_configuration>::
                    traits::letters_per_unit;

            const std::uint64_t derived_unit_count =
                (static_cast<std::uint64_t>(in_bp_size)
                    + letters_per_unit - 1)
                / letters_per_unit;
            const std::uint64_t derived_padding =
                (in_bp_size == 0) ? std::uint64_t{0}
                : (derived_unit_count * letters_per_unit)
                    - static_cast<std::uint64_t>(in_bp_size);
            ASSERT(static_cast<std::uint64_t>(in_unit_count) == derived_unit_count,
                   fmt::format("convert_from(recv): unit_count {} != "
                               "derived {} from bp_size {} lpu {}",
                               in_unit_count,
                               static_cast<std::size_t>(derived_unit_count),
                               static_cast<std::size_t>(in_bp_size),
                               letters_per_unit));
            ASSERT(static_cast<std::uint64_t>(in_padding_letters) == derived_padding,
                   fmt::format("convert_from(recv): padding {} != derived {} "
                               "from bp_size {} lpu {}",
                               static_cast<unsigned>(in_padding_letters),
                               static_cast<std::size_t>(derived_padding),
                               static_cast<std::size_t>(in_bp_size),
                               letters_per_unit));
            ASSERT(in_padding_letters < letters_per_unit
                   || (in_unit_count == 0 && in_padding_letters == 0),
                   fmt::format("convert_from(recv): padding {} >= lpu {}",
                               static_cast<unsigned>(in_padding_letters),
                               letters_per_unit));

            const std::size_t payload_bytes_value =
                static_cast<std::size_t>(in_unit_count)
                * p_storage_byte_width;
            const std::size_t total_bytes =
                header_bytes() + round_up_to_uint64(payload_bytes_value);

            compact_sequence_stdc* header =
                std::forward<p_allocator_type>(allocator)(total_bytes);
            if (header == nullptr) {
                result = nullptr;
                return;
            }
            header->vl_len_ = encode_varlena_header(total_bytes);
            header->alphabet_code =
                p_alphabet_set_type::template index_of<p_alphabet>();
            header->storage_unit_byte_width =
                static_cast<std::uint8_t>(p_storage_byte_width);
            header->padding_letters = in_padding_letters;
            header->bp_size = in_bp_size;
            header->encoding = bioseq::sequence_encoding::rank_packed;
            header->space = bioseq::cigar_space::compatibility;
            header->alignment_padding = {};

            auto* writer =
                reinterpret_cast<storage_unit_type*>(header->data);
            for (std::uint32_t unit_index = 0;
                 unit_index < in_unit_count;
                 ++unit_index) {
                writer[unit_index] = static_cast<storage_unit_type>(
                    receiver.template operator()<p_alphabet, std::uint64_t>(
                        receive_context,
                        static_cast<std::uint8_t>(p_storage_byte_width)));
            }
            result = header;
        });
    return result;
}

template<
    class p_alphabet_set_type,
    class p_base_pair_size_type,
    class p_varlena_header_type,
    class p_storage_width_set_type
>
requires (
    supported_alphabet_set<p_alphabet_set_type>
    && supported_storage_width_set<p_storage_width_set_type>
)
template<class p_allocator_type>
requires (stdc_allocator<p_allocator_type, compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>>)
inline compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>*
compact_sequence_stdc<
    p_alphabet_set_type,
    p_base_pair_size_type,
    p_varlena_header_type,
    p_storage_width_set_type>::recast_storage_width(
    std::uint8_t target_byte_width,
    p_allocator_type&& allocator) const
{
    ASSERT(
        p_storage_width_set_type::contains_runtime(
            target_byte_width),
        fmt::format(
            "recast_storage_width: target_byte_width {} is not enabled",
                    static_cast<unsigned>(target_byte_width)));

    if (encoding == bioseq::sequence_encoding::cigar_rle) {
        if (target_byte_width != storage_unit_byte_width) return nullptr;
        const auto total_bytes = header_bytes() + payload_bytes();
        auto* copy = std::forward<p_allocator_type>(allocator)(total_bytes);
        if (copy != nullptr) std::memcpy(copy, this, total_bytes);
        return copy;
    }
    if (encoding != bioseq::sequence_encoding::rank_packed) return nullptr;

    std::uint16_t new_padding = 0;
    std::size_t   payload_bytes_value = 0;
    convert_to([this,
                &target_byte_width,
                &new_padding,
                &payload_bytes_value]<class p_instance_type>(
                   const compact_sequence_range<p_instance_type>&) {
        if constexpr (p_instance_type::is_bit_packed) {
            p_storage_width_set_type::dispatch(
                target_byte_width,
                [this, &new_padding, &payload_bytes_value]<std::size_t p_target_storage_byte_width>() {
                    constexpr std::size_t target_letters_per_unit =
                        (p_target_storage_byte_width * 8) / p_instance_type::letter_bit_width;
                    const std::uint64_t derived_units = bp_size == 0 ? std::uint64_t{0}
                        : (static_cast<std::uint64_t>(bp_size) + target_letters_per_unit - 1u)
                            / target_letters_per_unit;
                    new_padding = static_cast<std::uint16_t>(derived_units * target_letters_per_unit
                        - static_cast<std::uint64_t>(bp_size));
                    payload_bytes_value = static_cast<std::size_t>(derived_units) * p_target_storage_byte_width;
                });
        }
    });

    const std::size_t total_bytes =
        header_bytes() + round_up_to_uint64(payload_bytes_value);
    compact_sequence_stdc* destination =
        std::forward<p_allocator_type>(allocator)(total_bytes);
    if (destination == nullptr) return nullptr;
    destination->vl_len_ = encode_varlena_header(total_bytes);
    destination->alphabet_code = alphabet_code;
    destination->storage_unit_byte_width = target_byte_width;
    destination->bp_size = bp_size;
    destination->padding_letters = new_padding;
    destination->encoding = encoding;
    destination->space = space;
    destination->alignment_padding = {};

    if (payload_bytes_value > 0) {
        std::memcpy(destination->data, this->data, payload_bytes_value);
    }
    return destination;
}
