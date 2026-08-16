/**
 * @file storage/lazy_sequence_parts/access_plan.cppm
 * @brief Resident borrowing and admitted sequence-access planning.
 */
export module bioseq.storage.lazy_sequence:access_plan;

import std;
import :view_contract;
import :materialization;
import bioseq.common.checked_arithmetic;
import bioseq.storage.scanner;
import bioseq.storage.size;

export namespace bioseq::storage {

/** @brief Physical route selected for a consumer's sequence-access requirement. */
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

/** @brief Resolved access route, resident descriptor, and admission requirement. */
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

/// Borrow a narrower packed *byte representation* over already resident
/// storage.  The only cross-width shape required by current upload paths is
/// canonical u64 -> u32 DNA packing.  On little-endian hosts the low u32 word
/// of each u64 contains the earlier LSB-first symbols, so splitting each
/// resident word is byte-for-byte identical to the established packed wire
/// layout.
///
/// The returned descriptor does not change the effective type of the source
/// allocation.  Its `units` member may be copied/read only through
/// character/byte access (for example a Vulkan buffer upload). Dereferencing
/// it as `uint32_t*` would violate the C++ effective-type/aliasing rules.
/** @brief Borrow a byte-only u32 representation from compatible resident u64 storage. */
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

/** @brief Plan streaming, borrowing, or admission-backed materialization for a region. */
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
