/**
 * @file storage/lazy_sequence_parts/access_plan.cppm
 * @brief Chooses streaming, borrowed storage, or a reserved packed copy.
 */
export module bioseq.storage.lazy_sequence:access_plan;

import std;
import bioseq.storage.lazy_sequence_view;
import :materialization;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.storage.scanner;
import bioseq.storage.size;

export namespace bioseq::storage {

/** How a consumer will read the requested sequence region. */
enum class sequence_access_route : std::uint8_t {
    stream_symbols,
    borrow_resident,
    materialize_admitted,
};

/// Controls whether borrowing may expose a different packed word width.
/// CPU readers use native_typed_storage_only. byte_upload_compatible also
/// permits compatible bytes to be copied to a GPU, without typed CPU loads.
enum class sequence_resident_borrow_policy : std::uint8_t {
    native_typed_storage_only,
    byte_upload_compatible,
};

/// Records how borrowed storage can be read.
/// reinterpreted_bytes permits byte access only. Its pointer must not be
/// dereferenced as an array of the requested word type.
enum class sequence_resident_borrow_kind : std::uint8_t {
    none,
    native_typed_storage,
    reinterpreted_bytes,
};

/** Selected access method, optional borrowed storage, and required memory reservation. */
template <sequence_alphabet p_alphabet> struct sequence_access_plan {
    using descriptor_type = typename lazy_sequence_view<p_alphabet>::resident_packed_sequence;

    sequence_access_requirement requirement{sequence_access_requirement::sequential_symbols};
    sequence_access_route route{sequence_access_route::stream_symbols};
    std::optional<descriptor_type> resident{};
    sequence_resident_borrow_kind resident_borrow_kind{sequence_resident_borrow_kind::none};
    std::uint64_t admission_bytes{};
    sequence_size_t from{};
    sequence_size_t to{};
    std::uint8_t requested_width{};
};

/**
 * Borrows a u32 byte layout from compatible resident u64 words.
 *
 * On little-endian hosts, splitting each u64 word preserves symbol order when
 * the rank width divides both word widths. The requested region must start
 * at a u32 boundary.
 *
 * The returned pointer still refers to u64 storage. Copy it through byte or
 * character access, as a GPU upload does. Typed uint32_t loads would violate
 * the source object's access rules. No allocation or ownership transfer occurs.
 */

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::optional<typename lazy_sequence_view<p_alphabet>::resident_packed_sequence>
try_borrow_reinterpreted_packed_bytes(lazy_sequence_view<p_alphabet> view,
                                      sequence_size_t from,
                                      sequence_size_t to,
                                      std::uint8_t requested_width) noexcept {
    using view_type = lazy_sequence_view<p_alphabet>;
    using descriptor_type = typename view_type::resident_packed_sequence;
    if constexpr (std::endian::native != std::endian::little) {
        return std::nullopt;
    } else {
        constexpr std::uint8_t k_source_width = sizeof(std::uint64_t);
        constexpr std::uint8_t k_target_width = sizeof(std::uint32_t);
        constexpr std::uint8_t k_bits_per_symbol =
            static_cast<std::uint8_t>(lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol);
        if (requested_width != k_target_width || from > to || to > view.bp_size()) {
            return std::nullopt;
        }
        if (k_bits_per_symbol == 0u ||
            (k_source_width * std::numeric_limits<unsigned char>::digits) % k_bits_per_symbol != 0u ||
            (k_target_width * std::numeric_limits<unsigned char>::digits) % k_bits_per_symbol != 0u) {
            return std::nullopt;
        }
        const sequence_size_t target_letters_per_unit =
            packed_letters_per_unit(k_target_width, k_bits_per_symbol);
        if (target_letters_per_unit == 0u) {
            return std::nullopt;
        }
        auto source = view.try_resident_packed(from, to, k_source_width);
        if (!source.has_value() ||
            source->bit_order != view_type::packed_symbol_order::least_significant_symbol_first ||
            source->bits_per_symbol != k_bits_per_symbol ||
            source->leading_symbol_offset % target_letters_per_unit != 0u) {
            return std::nullopt;
        }
        const sequence_size_t length = to - from;
        if (length == 0u) {
            return descriptor_type{
                .units = nullptr,
                .unit_count = 0u,
                .logical_length = 0u,
                .storage_unit_width = k_target_width,
                .bits_per_symbol = k_bits_per_symbol,
                .leading_symbol_offset = 0u,
                .trailing_padding = 0u,
                .bit_order = view_type::packed_symbol_order::least_significant_symbol_first,
            };
        }
        const sequence_size_t target_units_u64 =
            bioseq::common::ceil_div(length, target_letters_per_unit);
        const auto represented_symbols =
            bioseq::common::checked_mul(target_units_u64, target_letters_per_unit);
        if (target_units_u64 >
                static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)()) ||
            !represented_symbols.has_value()) {
            return std::nullopt;
        }
        const std::size_t halfword_offset =
            static_cast<std::size_t>(source->leading_symbol_offset / target_letters_per_unit);
        const std::size_t target_units = static_cast<std::size_t>(target_units_u64);
        const auto source_bytes = bioseq::common::checked_mul(
            source->unit_count, static_cast<std::size_t>(k_source_width));
        const auto byte_offset =
            bioseq::common::checked_mul(halfword_offset, static_cast<std::size_t>(k_target_width));
        const auto target_bytes =
            bioseq::common::checked_mul(target_units, static_cast<std::size_t>(k_target_width));
        if (!source_bytes.has_value() || !byte_offset.has_value() || !target_bytes.has_value()) {
            return std::nullopt;
        }
        if (*byte_offset > *source_bytes || *target_bytes > *source_bytes - *byte_offset) {
            return std::nullopt;
        }
        const sequence_size_t trailing_u64 = *represented_symbols - length;
        if (trailing_u64 >
            static_cast<sequence_size_t>((std::numeric_limits<std::uint16_t>::max)())) {
            return std::nullopt;
        }
        const auto* source_bytes_ptr = static_cast<const std::uint8_t*>(source->units);
        const void* target_units_ptr = source_bytes_ptr + *byte_offset;
        if (reinterpret_cast<std::uintptr_t>(target_units_ptr) % k_target_width != 0u) {
            return std::nullopt;
        }
        return descriptor_type{
            .units = target_units_ptr,
            .unit_count = target_units,
            .logical_length = length,
            .storage_unit_width = k_target_width,
            .bits_per_symbol = k_bits_per_symbol,
            .leading_symbol_offset = 0u,
            .trailing_padding = static_cast<std::uint16_t>(trailing_u64),
            .bit_order = view_type::packed_symbol_order::least_significant_symbol_first,
        };
    }
}

/**
 * Plans access to [from, to) without allocating or reserving memory.
 *
 * Sequential access streams ranks. A contiguous request first tries compatible
 * resident storage with no leading symbols. Other requests need a packed copy
 * and report the bytes to reserve before creating it.
 * @throws std::invalid_argument If the range or required word width is invalid.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] sequence_access_plan<p_alphabet>
plan_sequence_access(lazy_sequence_view<p_alphabet> view,
                     sequence_access_requirement requirement,
                     sequence_size_t from,
                     sequence_size_t to,
                     std::uint8_t requested_width = 0u,
                     sequence_resident_borrow_policy borrow_policy =
                         sequence_resident_borrow_policy::native_typed_storage_only) {
    if (from > to || to > view.bp_size()) {
        throw std::invalid_argument("invalid sequence access range");
    }
    sequence_access_plan<p_alphabet> plan{
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
    if (!(requested_width == 1u || requested_width == 2u || requested_width == 4u ||
          requested_width == 8u)) {
        throw std::invalid_argument("resident sequence access requires width 1, 2, 4, or 8");
    }
    if (requirement == sequence_access_requirement::contiguous_resident) {
        plan.resident = view.try_resident_packed(from, to, requested_width);
        if (plan.resident.has_value()) {
            plan.resident_borrow_kind = sequence_resident_borrow_kind::native_typed_storage;
        } else if (borrow_policy == sequence_resident_borrow_policy::byte_upload_compatible) {
            plan.resident = try_borrow_reinterpreted_packed_bytes(view, from, to, requested_width);
            if (plan.resident.has_value()) {
                plan.resident_borrow_kind = sequence_resident_borrow_kind::reinterpreted_bytes;
            }
        }
        if (plan.resident.has_value() && plan.resident->leading_symbol_offset == 0u) {
            plan.route = sequence_access_route::borrow_resident;
            return plan;
        }
        plan.resident.reset();
        plan.resident_borrow_kind = sequence_resident_borrow_kind::none;
    }
    plan.route = sequence_access_route::materialize_admitted;
    plan.admission_bytes = packed_sequence_admission_bytes<p_alphabet>(to - from, requested_width);
    return plan;
}

} // namespace bioseq::storage
