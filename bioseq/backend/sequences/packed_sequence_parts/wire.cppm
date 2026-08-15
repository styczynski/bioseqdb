/**
 * @file wire.cppm
 * @brief Standard-layout compact-sequence wire storage and factories.
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
import bioseq.sequences.rank_pack_core;
import :packed_sequence_core;
import :packed_sequence_packing;
import :packed_sequence_range;

export {

// The sequences module owns the storage-neutral compact header layout;
// PostgreSQL bindings provide allocation and wire callbacks without changing
// the packed representation.

namespace compact_sequence_stdc_detail {

/**
 * @brief Selects a standard-layout field for an alignment deficit.
 * @tparam p_deficit Padding bytes required before the payload.
 * @return Type identity with exactly the requested byte width.
 */
template<std::size_t p_deficit>
consteval auto pick_alignment_reserved_type() {
    if constexpr (p_deficit == 0) return std::type_identity<void>{};
    else if constexpr (p_deficit == 1) return std::type_identity<std::uint8_t>{};
    else if constexpr (p_deficit == 2) return std::type_identity<std::uint16_t>{};
    else if constexpr (p_deficit == 4) return std::type_identity<std::uint32_t>{};
    else return std::type_identity<std::array<std::uint8_t, p_deficit>>{};
}

/**
 * @brief Computes padding needed to align payload bytes to 64 bits.
 * @tparam p_varlena_bytes Varlena header width.
 * @tparam p_code_bytes Alphabet code width.
 * @tparam p_bp_size_bytes Biological length field width.
 * @return Required padding bytes in the fixed header.
 */
template<size_t p_varlena_bytes, size_t p_code_bytes, size_t p_bp_size_bytes>
consteval size_t reserved_deficit() {
    constexpr size_t fixed_bytes =
        p_varlena_bytes + p_code_bytes
        + 1u /* storage_unit_byte_width */
        + 2u /* padding_letters */
        + p_bp_size_bytes;
    constexpr size_t target = 8u;
    return (target - (fixed_bytes % target)) % target;
}

/** @brief Field type occupying one fixed-header alignment deficit. */
template <std::size_t p_deficit>
using alignment_reserved_for_type =
    typename decltype(pick_alignment_reserved_type<p_deficit>())::type;

/** @brief Stores the fixed-header bytes required before the payload. */
template<std::size_t p_deficit>
struct alignment_reserved_holder {
    alignment_reserved_for_type<p_deficit> reserved;
};
/** @brief Omits alignment storage when the fixed header is already aligned. */
template<>
struct alignment_reserved_holder<0> { };

}  // namespace compact_sequence_stdc_detail

/**
 * @brief Runtime type parameters for a standard-layout compact sequence.
 *
 * The encoded representation stores the alphabet code in the next byte and
 * the base-two logarithm of the storage-unit byte width in the low byte.
 */
struct compact_sequence_stdc_type_params {
    /** @brief Index of the alphabet in the owning supported-alphabet set. */
    std::uint8_t alphabet_code;

    /** @brief Width of one packed storage unit in bytes. */
    std::uint8_t storage_unit_byte_width;

    /**
     * @brief Encode the parameters as a PostgreSQL type modifier.
     * @return The compact signed 32-bit type modifier.
     */
    [[nodiscard]] constexpr std::int32_t encode() const noexcept {
        const std::uint8_t width_code =
              storage_unit_byte_width == 1 ? 0u
            : storage_unit_byte_width == 2 ? 1u
            : storage_unit_byte_width == 4 ? 2u
            : storage_unit_byte_width == 8 ? 3u
            : 0xFFu;
        DEBUG_ASSERT(width_code != 0xFFu,
                     fmt::format("encode: storage_unit_byte_width {} invalid",
                                 static_cast<unsigned>(storage_unit_byte_width)));
        return (static_cast<std::int32_t>(alphabet_code) << 8) | width_code;
    }

    /**
     * @brief Decode a PostgreSQL type modifier.
     * @param typmod Encoded type modifier produced by encode().
     * @return Decoded alphabet and storage-width parameters.
     */
    [[nodiscard]] static constexpr compact_sequence_stdc_type_params
    decode(std::int32_t typmod) noexcept {
        const std::uint8_t width_code =
            static_cast<std::uint8_t>(typmod & 0xFF);
        DEBUG_ASSERT(width_code <= 3u,
                     fmt::format("decode: width_code {} > 3",
                                 static_cast<unsigned>(width_code)));
        return {
            .alphabet_code           = static_cast<std::uint8_t>((typmod >> 8) & 0xFF),
            .storage_unit_byte_width = static_cast<std::uint8_t>(1u << width_code),
        };
    }

    /** @brief Compares alphabet and storage-width parameters. */
    [[nodiscard]] constexpr bool operator==(
        const compact_sequence_stdc_type_params&) const = default;
};

/**
 * @brief Allocator for a standard-layout compact sequence allocation.
 *
 * A conforming allocator accepts a byte count and returns suitably aligned
 * storage convertible to `p_allocated_type*`, or `nullptr` on failure.
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
 * @brief Compile-time specialization of a runtime compact-sequence header.
 * @tparam p_stdc_type Standard-layout compact-sequence header type.
 * @tparam p_alphabet Pinned alphabet.
 * @tparam p_storage_unit_byte_width Pinned packed-unit width in bytes.
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
 * @brief Stores a C-compatible packed-sequence header and inline payload.
 *
 * The fixed header aligns `data` to a 64-bit boundary. Runtime alphabet and
 * storage-width fields dispatch to an exact `compact_sequence_stdc_instance`.
 *
 * @tparam p_alphabet_set_type Closed set of supported alphabets.
 * @tparam p_base_pair_size_type Integer type storing biological length.
 * @tparam p_varlena_header_type Integer type storing the varlena header.
 * @tparam p_storage_width_set_type Closed set of supported storage widths.
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
            sizeof(varlena_header_t),
            sizeof(alphabet_code_t),
            sizeof(bp_size_t)
        >();

    /** @brief PostgreSQL four-byte uncompressed varlena header. */
    varlena_header_t      vl_len_;
    /** @brief Alphabet index in supported_alphabets_type. */
    alphabet_code_t       alphabet_code;
    /** @brief Width of one packed storage unit in bytes. */
    std::uint8_t          storage_unit_byte_width;
    /** @brief Unused alphabet slots in the final packed unit. */
    std::uint16_t         padding_letters;
    /** @brief Number of logical alphabet symbols. */
    bp_size_t             bp_size;
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
     * @brief Encodes a four-byte uncompressed PostgreSQL varlena length.
     * @param total_bytes Complete allocation size including the fixed header.
     * @return Length shifted above the two varlena format bits.
     */
    [[nodiscard]] static constexpr varlena_header_t
    encode_varlena_header(std::size_t total_bytes) noexcept {
        return static_cast<varlena_header_t>(
            static_cast<std::uint32_t>(total_bytes) << 2);
    }

    /**
     * @brief Dispatches runtime alphabet and storage width to one exact instance.
     * @tparam p_handler_type Handler invoked with an instance template argument.
     * @param alpha_code Runtime alphabet index.
     * @param byte_width Runtime packed-unit width.
     * @param handler Handler invoked exactly once.
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
                p_storage_width_set_type::dispatch(
                    byte_width, [&handler]<size_t p_dispatched_byte_width>() {
                        handler.template operator()<
                            compact_sequence_stdc_instance<
                                compact_sequence_stdc,
                                p_dispatched_alphabet,
                                p_dispatched_byte_width>
                        >();
                    });
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
     * @brief Receives validated packed units into a new standard-layout value.
     * @tparam p_alphabet_type Logical sequence alphabet.
     * @param target_storage_byte_width Packed-unit width in the incoming stream.
     * @param in_padding_letters Unused rank slots in the final unit.
     * @param in_bp_size Biological symbol count.
     * @param in_unit_count Packed-unit count.
     * @param receive_context Context passed to the receiver.
     * @param receiver Typed packed-unit receiver.
     * @param allocator Allocation callback for the complete value.
     * @return Allocated value, or null when allocation fails.
     */
    template<
        sequence_alphabet p_alphabet_type,
        class p_receive_context_type,
        class p_receiver_type,
        class p_allocator_type
    >
    requires (
        alphabet_in_supported_set<p_alphabet_set_type, p_alphabet_type>
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
     * @brief Allocates an empty header with a requested packed-unit capacity.
     * @tparam p_alphabet_type Logical sequence alphabet.
     * @param target_storage_byte_width Packed-unit width in bytes.
     * @param unit_count_value Reserved packed-unit count.
     * @param allocator Allocation callback for the complete value.
     * @return Allocated empty value, or null when allocation fails.
     */
    template<
        sequence_alphabet p_alphabet_type,
        class p_allocator_type
    >
    requires (
        alphabet_in_supported_set<p_alphabet_set_type, p_alphabet_type>
        && stdc_allocator<p_allocator_type, compact_sequence_stdc>
    )
    [[nodiscard]]
    static compact_sequence_stdc* reserve(
        std::uint8_t target_storage_byte_width,
        std::uint32_t unit_count_value,
        p_allocator_type&& allocator);

    /**
     * @brief Sends every packed unit through a width-aware callback.
     * @param send_context Context passed to the sender.
     * @param sender Typed packed-unit sender.
     */
    template<class p_send_context_type, class p_sender_type>
    void send_to(
        p_send_context_type& send_context,
        p_sender_type&& sender) const;

    /**
     * @brief Hands a typed zero-copy range to one dispatched handler.
     * @param handler Handler invoked with the exact instance type and range.
     */
    template<class p_handler_type>
    [[gnu::always_inline]]
    void convert_to(p_handler_type&& handler) const;

    /**
     * @brief Recasts packed bytes to another compatible storage-unit width.
     * @param target_byte_width Destination packed-unit width.
     * @param allocator Allocation callback for the destination value.
     * @return Fresh value with unchanged payload bytes, or null on allocation failure.
     */
    template<class p_allocator_type>
    requires (stdc_allocator<p_allocator_type, compact_sequence_stdc>)
    [[nodiscard]]
    compact_sequence_stdc* recast_storage_width(
        std::uint8_t target_byte_width,
        p_allocator_type&& allocator) const;

    /** @brief Returns the logical packed-unit count for the runtime shape. */
    [[nodiscard]] size_t unit_count() const noexcept;

    /** @brief Returns payload bytes excluding trailing allocation alignment. */
    [[nodiscard]] size_t payload_bytes() const noexcept {
        return unit_count() * storage_unit_byte_width;
    }

    /** @brief Return the runtime alphabet and storage-width parameters. */
    [[nodiscard]] constexpr compact_sequence_stdc_type_params
    type_parameters() const noexcept {
        return {
            .alphabet_code           = static_cast<std::uint8_t>(alphabet_code),
            .storage_unit_byte_width = storage_unit_byte_width,
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
}  // namespace compact_sequence_stdc_detail

/**
 * @brief Pins one runtime compact header to an alphabet and storage width.
 * @tparam p_stdc_type Standard-layout compact header.
 * @tparam p_alphabet Logical sequence alphabet.
 * @tparam p_storage_unit_byte_width Packed-unit width in bytes.
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
    // Fixed-width standard types keep Highway lane selection identical across
    // LP64 platforms, where Boost's least-width type may choose another C++
    // spelling with the same bit width.
    using storage_unit =
        std::conditional_t<p_storage_unit_byte_width == 8, std::uint64_t,
        std::conditional_t<p_storage_unit_byte_width == 4, std::uint32_t,
        std::conditional_t<p_storage_unit_byte_width == 2, std::uint16_t,
                                                          std::uint8_t>>>;

    static constexpr size_t byte_width        = p_storage_unit_byte_width;
    static constexpr size_t letter_bit_width  =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);

    // The pack lookup pins the display name at the type level.
    static constexpr auto alphabet_name =
        p_stdc_type::supported_alphabets_type::template name_of<p_alphabet>();
    static constexpr std::string_view alphabet_name_sv = alphabet_name.view();

    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/**
 * @brief Borrows a typed zero-copy range from a standard-layout compact value.
 * @tparam p_instance_type Header type pinned to one alphabet and storage width.
 */
template<typename p_instance_type>
requires (is_compact_sequence_stdc_instance<p_instance_type>)
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
     * @brief Construct a zero-copy typed view over a compact header.
     * @param storage Header and packed payload whose lifetime outlives the view.
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

    /** @brief Tests biological length and packed payload equality. */
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

// Disambiguation: the instance type must NOT also satisfy the two
// concepts that drive the existing compact_sequence_range partial
// specialisations. A future edit that broke this would manifest as an
// "ambiguous partial specialisation" diagnostic at every use site;
// catch it here with a one-line message instead.
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
            constexpr std::size_t letters_per_unit =
                compact_sequence_range<p_instance_type>::traits::letters_per_unit;
            result = (bp_size == 0) ? std::size_t{0}
                : (bp_size + letters_per_unit - 1) / letters_per_unit;
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

    std::uint16_t new_padding = 0;
    std::size_t   payload_bytes_value = 0;
    convert_to([this,
                &target_byte_width,
                &new_padding,
                &payload_bytes_value]<class p_instance_type>(
                   const compact_sequence_range<p_instance_type>&) {
        p_storage_width_set_type::dispatch(
            target_byte_width,
            [this, &new_padding, &payload_bytes_value]<std::size_t p_target_storage_byte_width>() {
                constexpr std::size_t target_letters_per_unit =
                    (p_target_storage_byte_width * 8)
                    / p_instance_type::letter_bit_width;
                const std::uint64_t derived_units = (bp_size == 0) ? std::uint64_t{0}
                    : (static_cast<std::uint64_t>(bp_size)
                        + target_letters_per_unit - 1)
                        / target_letters_per_unit;
                new_padding   = static_cast<std::uint16_t>(
                    (bp_size == 0) ? 0u
                    : (derived_units * target_letters_per_unit
                        - static_cast<std::uint64_t>(bp_size)));
                payload_bytes_value =
                    static_cast<std::size_t>(derived_units)
                    * p_target_storage_byte_width;
            });
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

    if (payload_bytes_value > 0) {
        std::memcpy(destination->data, this->data, payload_bytes_value);
    }
    return destination;
}
