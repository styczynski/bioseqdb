/**
 * @file storage/lazy_sequence_parts/materialization.cppm
 * @brief Admission-accounted packed-sequence materialization.
 */
module;

#include <hwy/aligned_allocator.h>

export module bioseq.storage.lazy_sequence:materialization;

import std;
import bioseq.storage.lazy_sequence_view;
import :transforms;
import bioseq.common.checked_arithmetic;
import bioseq.common.seqan_adapter;
import bioseq.parallelism.memory_admission;
import bioseq.storage.coordinates;
import bioseq.storage.scanner;
import bioseq.storage.size;
import bioseq.sequences;
import bioseq.sequences.rank_pack;

export namespace bioseq::storage {

namespace lazy_sequence_detail {

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

    [[nodiscard]] std::uint8_t* data() noexcept {
        return data_.get();
    }
    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return data_.get();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

  private:
    hwy::AlignedFreeUniquePtr<std::uint8_t[]> data_{};
    std::size_t size_{};
};

template <sequence_alphabet p_alphabet> struct resident_view_ops_context {
    using descriptor_type = typename lazy_sequence_view<p_alphabet>::resident_packed_sequence;
    descriptor_type const* descriptor{};
};

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint64_t load_resident_unit(resident_view_ops_context<p_alphabet> const& context,
                                               std::size_t unit_index) noexcept {
    const auto& descriptor = *context.descriptor;
    const auto* bytes = static_cast<const std::uint8_t*>(descriptor.units) +
                        unit_index * descriptor.storage_unit_width;
    switch (descriptor.storage_unit_width) {
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

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint8_t resident_rank_at(resident_view_ops_context<p_alphabet> const& context,
                                            sequence_size_t position) noexcept {
    const auto& descriptor = *context.descriptor;
    const sequence_size_t letters_per_unit =
        descriptor.storage_unit_width * std::numeric_limits<unsigned char>::digits /
        descriptor.bits_per_symbol;
    const sequence_size_t slot = descriptor.leading_symbol_offset + position;
    const std::size_t unit_index = static_cast<std::size_t>(slot / letters_per_unit);
    const auto shift =
        static_cast<unsigned>((slot % letters_per_unit) * descriptor.bits_per_symbol);
    const std::uint64_t mask = (std::uint64_t{1} << descriptor.bits_per_symbol) - 1u;
    return static_cast<std::uint8_t>((load_resident_unit(context, unit_index) >> shift) & mask);
}

template <bool p_is_reversed, bool p_write_reversed, sequence_alphabet p_alphabet>
[[nodiscard]] const std::uint8_t* resident_read_at(void const* state,
                                                   sequence_size_t bp_from,
                                                   std::size_t required_length,
                                                   std::span<std::uint8_t> scratch) {
    const auto& context = *static_cast<const resident_view_ops_context<p_alphabet>*>(state);
    const sequence_size_t total = context.descriptor->logical_length;
    if (bp_from > total || required_length > total - bp_from || scratch.size() < required_length) {
        abort_lazy_sequence_access("resident packed sequence rank range is invalid");
    }
    if (required_length == 0u) {
        return scratch.data();
    }
    if constexpr (p_is_reversed && !complementable_sequence_alphabet<p_alphabet>) {
        throw std::invalid_argument(
            "reverse-complement encoding requires a complementable alphabet");
    }
    const sequence_size_t forward_from =
        p_is_reversed ? total - bp_from - required_length : bp_from;
    constexpr bool k_is_descending_write =
        (p_is_reversed && !p_write_reversed) || (!p_is_reversed && p_write_reversed);
    for (std::size_t index = 0u; index < required_length; ++index) {
        std::uint8_t rank = resident_rank_at(context, forward_from + index);
        if constexpr (p_is_reversed && complementable_sequence_alphabet<p_alphabet>) {
            rank = lazy_sequence_traits<p_alphabet>::letter_complement(rank);
        }
        scratch[k_is_descending_write ? required_length - 1u - index : index] = rank;
    }
    return scratch.data();
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] sequence_size_t resident_bp_size(void const* state) noexcept {
    const auto& context = *static_cast<const resident_view_ops_context<p_alphabet>*>(state);
    return context.descriptor->logical_length;
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] auto resident_probe(void const* state,
                                  sequence_size_t from,
                                  sequence_size_t to,
                                  std::uint8_t requested_width) noexcept ->
    typename lazy_sequence_view<p_alphabet>::resident_packed_result {
    using view_type = lazy_sequence_view<p_alphabet>;
    using descriptor_type = typename view_type::resident_packed_sequence;
    const auto& context = *static_cast<const resident_view_ops_context<p_alphabet>*>(state);
    const descriptor_type& source = *context.descriptor;
    if (from > to || to > source.logical_length || requested_width != source.storage_unit_width) {
        return std::nullopt;
    }
    const sequence_size_t length = to - from;
    if (length == 0u) {
        return descriptor_type{
            .units = nullptr,
            .unit_count = 0u,
            .logical_length = 0u,
            .storage_unit_width = source.storage_unit_width,
            .bits_per_symbol = source.bits_per_symbol,
            .leading_symbol_offset = 0u,
            .trailing_padding = 0u,
            .bit_order = source.bit_order,
        };
    }

    const sequence_size_t letters_per_unit =
        source.storage_unit_width * std::numeric_limits<unsigned char>::digits /
        source.bits_per_symbol;
    const auto absolute_slot = bioseq::common::checked_add(
        static_cast<sequence_size_t>(source.leading_symbol_offset), from);
    if (!absolute_slot.has_value()) {
        return std::nullopt;
    }
    const sequence_size_t first_unit = *absolute_slot / letters_per_unit;
    const sequence_size_t leading = *absolute_slot % letters_per_unit;
    const auto occupied = bioseq::common::checked_add(leading, length);
    if (!occupied.has_value()) {
        return std::nullopt;
    }
    const sequence_size_t unit_count = bioseq::common::ceil_div(*occupied, letters_per_unit);
    const auto represented_symbols = bioseq::common::checked_mul(unit_count, letters_per_unit);
    if (first_unit > source.unit_count || unit_count > source.unit_count - first_unit ||
        !represented_symbols.has_value() ||
        unit_count > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
        return std::nullopt;
    }
    const auto* units =
        static_cast<const std::uint8_t*>(source.units) + first_unit * source.storage_unit_width;
    return descriptor_type{
        .units = units,
        .unit_count = static_cast<std::size_t>(unit_count),
        .logical_length = length,
        .storage_unit_width = source.storage_unit_width,
        .bits_per_symbol = source.bits_per_symbol,
        .leading_symbol_offset = static_cast<std::uint16_t>(leading),
        .trailing_padding = static_cast<std::uint16_t>(*represented_symbols - *occupied),
        .bit_order = source.bit_order,
    };
}

[[nodiscard]] inline bool resident_worker_shareable(void const*) noexcept {
    return true;
}

template <sequence_alphabet p_alphabet> struct resident_adapter {
    using view_type = lazy_sequence_view<p_alphabet>;
    static inline const typename view_type::backend_ops k_ops{
        .bp_size = &resident_bp_size<p_alphabet>,
        .read_at =
            {
                &resident_read_at<false, false, p_alphabet>,
                &resident_read_at<false, true, p_alphabet>,
                &resident_read_at<true, false, p_alphabet>,
                &resident_read_at<true, true, p_alphabet>,
            },
        .try_resident_packed = &resident_probe<p_alphabet>,
        .copy_packed_sequence = nullptr,
        .worker_shareable = &resident_worker_shareable,
    };
};

template <sequence_alphabet p_alphabet, class p_storage_unit>
[[nodiscard]] bioseq::sequences::rank_pack_result
pack_view_region_into(lazy_sequence_view<p_alphabet> view,
                      sequence_size_t from,
                      sequence_size_t to,
                      void* output,
                      std::size_t unit_count) {
    constexpr std::size_t k_bits_per_symbol =
        lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol;
    using output_instance = sequence_custom_alphabet<p_alphabet, p_storage_unit, k_bits_per_symbol>;
    using unit_sink = bioseq::sequences::fixed_rank_pack_unit_sink<p_storage_unit>;
    using packer = bioseq::sequences::checked_rank_pack_sink<
        output_instance,
        bioseq::sequences::invalid_rank_policy::reject,
        bioseq::sequences::pack_capacity_policy::fixed_span,
        unit_sink>;

    auto units = std::span<p_storage_unit>(static_cast<p_storage_unit*>(output), unit_count);
    packer writer{unit_sink{units}};
    stream_transformed_ranks<sequence_transform::identity>(view, from, to, writer);
    return writer.finish(to - from, unit_count);
}

} // namespace lazy_sequence_detail

template <sequence_alphabet p_alphabet> class admitted_packed_sequence;

/** @brief Return the retained-byte budget required for packed materialization. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint64_t packed_sequence_admission_bytes(sequence_size_t logical_length,
                                                            std::uint8_t requested_width);

template <sequence_alphabet p_alphabet>
[[nodiscard]] admitted_packed_sequence<p_alphabet>
materialize_packed_sequence(lazy_sequence_view<p_alphabet> view,
                            sequence_size_t from,
                            sequence_size_t to,
                            std::uint8_t requested_width,
                            bioseq::parallelism::memory_permit&& permit);

/** @brief Move-only owner of an admission-accounted aligned packed sequence. */
template <sequence_alphabet p_alphabet> class admitted_packed_sequence {
  public:
    using view_type = lazy_sequence_view<p_alphabet>;
    using descriptor_type = typename view_type::resident_packed_sequence;

    admitted_packed_sequence(admitted_packed_sequence&&) noexcept = default;
    admitted_packed_sequence& operator=(admitted_packed_sequence&&) = delete;
    admitted_packed_sequence(admitted_packed_sequence const&) = delete;
    admitted_packed_sequence& operator=(admitted_packed_sequence const&) = delete;

    [[nodiscard]] descriptor_type const& descriptor() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("access to moved admitted packed region");
        }
        return state_->descriptor_;
    }

    [[nodiscard]] view_type view() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("view of moved admitted packed region");
        }
        using adapter_type = lazy_sequence_detail::resident_adapter<p_alphabet>;
        return view_type{std::addressof(state_->view_context_),
                         std::addressof(adapter_type::k_ops)};
    }

#ifdef BIOSEQ_TEST_INDEXES
    [[nodiscard]] std::uint64_t test_admission_bytes() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("admission query on moved packed region");
        }
        return state_->permit_.bytes();
    }

    [[nodiscard]] std::uintptr_t test_storage_address() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("storage query on moved packed region");
        }
        return reinterpret_cast<std::uintptr_t>(state_->descriptor_.units);
    }
#endif

  private:
    struct stable_state {
        bioseq::parallelism::memory_permit permit_;
        lazy_sequence_detail::aligned_packed_storage units_;
        descriptor_type descriptor_;
        lazy_sequence_detail::resident_view_ops_context<p_alphabet> view_context_;

        stable_state(bioseq::parallelism::memory_permit&& permit,
                     std::size_t allocation_bytes,
                     descriptor_type descriptor)
            : permit_(std::move(permit)),
              units_(lazy_sequence_detail::aligned_packed_storage::allocate(allocation_bytes)),
              descriptor_(descriptor), view_context_{std::addressof(descriptor_)} {
            descriptor_.units = units_.data();
        }
    };

    explicit admitted_packed_sequence(std::unique_ptr<stable_state> state) noexcept
        : state_(std::move(state)) {}

    template <sequence_alphabet p_other_alphabet>
    friend admitted_packed_sequence<p_other_alphabet>
    materialize_packed_sequence(lazy_sequence_view<p_other_alphabet>,
                                sequence_size_t,
                                sequence_size_t,
                                std::uint8_t,
                                bioseq::parallelism::memory_permit&&);

    template <sequence_alphabet p_other_alphabet>
    friend std::uint64_t packed_sequence_admission_bytes(sequence_size_t, std::uint8_t);

    std::unique_ptr<stable_state> state_;
};

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint64_t packed_sequence_admission_bytes(sequence_size_t logical_length,
                                                            std::uint8_t requested_width) {
    using owner_type = admitted_packed_sequence<p_alphabet>;
    if (!(requested_width == 1u || requested_width == 2u || requested_width == 4u ||
          requested_width == 8u)) {
        throw std::invalid_argument("invalid packed storage-unit width");
    }
    const sequence_size_t letters_per_unit = packed_letters_per_unit(
        requested_width,
        static_cast<std::uint8_t>(lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol));
    const sequence_size_t unit_count = bioseq::common::ceil_div(logical_length, letters_per_unit);
    const auto allocation_bytes =
        bioseq::common::checked_mul(unit_count, static_cast<sequence_size_t>(requested_width));
    if (!allocation_bytes.has_value() ||
        unit_count > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)()) ||
        *allocation_bytes >
            static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error("packed materialization size overflow");
    }
    const auto admitted_bytes = bioseq::common::checked_add(
        *allocation_bytes, static_cast<std::uint64_t>(sizeof(typename owner_type::stable_state)));
    if (!admitted_bytes.has_value()) {
        throw std::overflow_error("packed materialization admission overflow");
    }
    return *admitted_bytes;
}

/** @brief Materialize a planned region into permit-backed packed storage. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] admitted_packed_sequence<p_alphabet>
materialize_packed_sequence(lazy_sequence_view<p_alphabet> view,
                            sequence_size_t from,
                            sequence_size_t to,
                            std::uint8_t requested_width,
                            bioseq::parallelism::memory_permit&& permit) {
    using owner_type = admitted_packed_sequence<p_alphabet>;
    using descriptor_type = typename owner_type::descriptor_type;
    using bit_order_type = typename lazy_sequence_view<p_alphabet>::packed_symbol_order;
    constexpr std::uint8_t k_bits_per_symbol =
        static_cast<std::uint8_t>(lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol);

    if (from > to || to > view.bp_size()) {
        throw std::invalid_argument("invalid lazy packed materialization range");
    }
    if (!(requested_width == 1u || requested_width == 2u || requested_width == 4u ||
          requested_width == 8u)) {
        throw std::invalid_argument("invalid packed storage-unit width");
    }
    if (to > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error("packed materialization coordinate overflow");
    }

    const sequence_size_t length = to - from;
    const sequence_size_t letters_per_unit =
        packed_letters_per_unit(requested_width, k_bits_per_symbol);
    const sequence_size_t unit_count = bioseq::common::ceil_div(length, letters_per_unit);
    const auto allocation_bytes_u64 =
        bioseq::common::checked_mul(unit_count, static_cast<sequence_size_t>(requested_width));
    const auto represented_symbols = bioseq::common::checked_mul(unit_count, letters_per_unit);
    if (!allocation_bytes_u64.has_value() || !represented_symbols.has_value() ||
        unit_count > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)()) ||
        *allocation_bytes_u64 >
            static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error("packed materialization size overflow");
    }
    const std::size_t allocation_bytes = static_cast<std::size_t>(*allocation_bytes_u64);
    const std::uint64_t admitted_bytes =
        packed_sequence_admission_bytes<p_alphabet>(length, requested_width);
    if (!permit || permit.bytes() < admitted_bytes) {
        throw std::length_error("packed materialization permit is smaller than retained storage");
    }

    const sequence_size_t trailing = *represented_symbols - length;
    descriptor_type descriptor{
        .units = nullptr,
        .unit_count = static_cast<std::size_t>(unit_count),
        .logical_length = length,
        .storage_unit_width = requested_width,
        .bits_per_symbol = k_bits_per_symbol,
        .leading_symbol_offset = 0u,
        .trailing_padding = static_cast<std::uint16_t>(trailing),
        .bit_order = bit_order_type::least_significant_symbol_first,
    };
    auto state = std::make_unique<typename owner_type::stable_state>(
        std::move(permit), allocation_bytes, descriptor);

    const bool bulk_copied = from == 0u && to == view.bp_size() &&
                             view.try_copy_packed_sequence(
                                 std::span<std::uint8_t>{state->units_.data(), allocation_bytes},
                                 static_cast<std::size_t>(unit_count),
                                 requested_width);
    if (!bulk_copied) {
        bioseq::sequences::rank_pack_result packed{};
        switch (requested_width) {
        case 1u:
            packed = lazy_sequence_detail::pack_view_region_into<p_alphabet, std::uint8_t>(
                view, from, to, state->units_.data(), static_cast<std::size_t>(unit_count));
            break;
        case 2u:
            packed = lazy_sequence_detail::pack_view_region_into<p_alphabet, std::uint16_t>(
                view, from, to, state->units_.data(), static_cast<std::size_t>(unit_count));
            break;
        case 4u:
            packed = lazy_sequence_detail::pack_view_region_into<p_alphabet, std::uint32_t>(
                view, from, to, state->units_.data(), static_cast<std::size_t>(unit_count));
            break;
        case 8u:
            packed = lazy_sequence_detail::pack_view_region_into<p_alphabet, std::uint64_t>(
                view, from, to, state->units_.data(), static_cast<std::size_t>(unit_count));
            break;
        default:
            throw std::invalid_argument("invalid packed storage-unit width");
        }
        if (packed.status != bioseq::sequences::rank_pack_status::ok ||
            packed.emitted_symbols != length || packed.written_units != unit_count ||
            packed.padding_letters != trailing) {
            throw std::runtime_error("checked lazy rank packing failed");
        }
    }
    (void)publish_sequence_allocation(view.payload_allocation_observer(),
                                      sequence_payload_allocation_kind::admitted_packed_sequence,
                                      length,
                                      length,
                                      static_cast<std::uint64_t>(allocation_bytes),
                                      admitted_bytes);
    return owner_type{std::move(state)};
}

static_assert(std::is_move_constructible_v<admitted_packed_sequence<seq::dna4>>);
static_assert(!std::is_move_assignable_v<admitted_packed_sequence<seq::dna4>>);
static_assert(!std::is_copy_constructible_v<admitted_packed_sequence<seq::dna4>>);

} // namespace bioseq::storage
