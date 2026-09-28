/**
 * @file storage/lazy_sequence_parts/materialization.cppm
 * @brief Creates packed sequence copies covered by memory reservations.
 */
module;

#include <hwy/aligned_allocator.h>
#include <new>

export module bioseq.storage.lazy_sequence:materialization;

import std;
import bioseq.common.heap_allocation;
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

/**
 * Bounds the allocation request for a Highway-aligned payload.
 *
 * Includes space for alignment and Highway's varying offset between allocations.
 * The allocation callback checks the actual request against this bound before
 * allocating, so a larger request fails instead of exceeding  the reserved plan.
 */
[[nodiscard]] inline std::size_t packed_allocator_request_bound(std::size_t bytes) {
    if (bytes == 0u) {
        return 0u;
    }
#if HWY_ARCH_RISCV && defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 11000
    constexpr std::size_t alignment = std::max<std::size_t>(HWY_ALIGNMENT, 4096u);
#else
    constexpr std::size_t alignment = HWY_ALIGNMENT;
#endif
#if HWY_ARCH_X86
    constexpr std::size_t alias = std::max<std::size_t>(alignment, 1024u);
#else
    constexpr std::size_t alias = alignment;
#endif
    static_assert(sizeof(void*) + sizeof(std::size_t) <= alignment);
    static_assert(alias % alignment == 0u);
    constexpr std::size_t maximum_offset =
        alias == alignment ? alignment : alias - alignment;
    const auto overhead = bioseq::common::checked_add(alias, maximum_offset);
    const auto request = overhead ? bioseq::common::checked_add(bytes, *overhead)
                                  : std::optional<std::size_t>{};
    if (!request || bytes >= (std::numeric_limits<std::size_t>::max)() / 2u) {
        throw std::overflow_error("packed aligned-allocation request overflow");
    }
    return *request;
}

class aligned_packed_storage {
    struct pending_allocation {
        std::size_t highway_request_bound;
        bioseq::common::heap_allocation_detail::heap_allocation_plan heap_plan;
        std::size_t allocated_bytes = 0u;
    };

    static void* allocate_under_bound(void* opaque, std::size_t requested_bytes) noexcept {
        auto& pending = *static_cast<pending_allocation*>(opaque);
        if (requested_bytes > pending.highway_request_bound || pending.allocated_bytes != 0u) {
            return nullptr;
        }
        // Allocate the full planned request so the backing allocation has the
        // same bound regardless of Highway's chosen offset.
        return bioseq::common::heap_allocation_detail::allocate_and_touch(
            pending.heap_plan, pending.allocated_bytes);
    }

  public:
    aligned_packed_storage() = default;
    aligned_packed_storage(aligned_packed_storage const&) = delete;
    aligned_packed_storage& operator=(aligned_packed_storage const&) = delete;
    aligned_packed_storage(aligned_packed_storage&&) noexcept = default;
    aligned_packed_storage& operator=(aligned_packed_storage&&) noexcept = default;

    [[nodiscard]] static aligned_packed_storage allocate(
        std::size_t bytes,
        std::size_t highway_request_bound,
        const bioseq::common::heap_allocation_detail::heap_allocation_plan& heap_plan) {
        aligned_packed_storage storage;
        if (bytes == 0u) {
            return storage;
        }
        pending_allocation pending{
            .highway_request_bound = highway_request_bound,
            .heap_plan = heap_plan,
        };
        // The deleter needs only the allocated pointer. The callback's temporary
        // state, remains on this stack and is not saved in the owner.
        storage.data_.reset(static_cast<std::uint8_t*>(
            hwy::AllocateAlignedBytes(bytes, &allocate_under_bound, std::addressof(pending))));
        if (storage.data_ == nullptr) {
            throw std::bad_alloc{};
        }
        if (pending.allocated_bytes < heap_plan.allocator_request_bytes ||
            pending.allocated_bytes > heap_plan.allocation_requirement_bytes) {
            throw std::logic_error("packed payload allocation escaped its physical plan");
        }
        storage.size_ = bytes;
        storage.allocation_bytes_ = pending.allocated_bytes;
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

    [[nodiscard]] std::size_t allocation_bytes() const noexcept {
        return allocation_bytes_;
    }

  private:
    hwy::AlignedFreeUniquePtr<std::uint8_t[]> data_{};
    std::size_t size_{};
    std::size_t allocation_bytes_{};
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

/** Allocation sizes and reservation bounds for the packed payload and its owner. */
struct packed_sequence_physical_plan {
    /// Bytes visible through the packed descriptor.
    std::uint64_t logical_payload_bytes = 0u;
    /// Upper bound on Highway's aligned-allocation request.
    std::uint64_t payload_highway_request_bound_bytes = 0u;
    /// Bytes requested from the allocator for the packed payload.
    std::uint64_t payload_allocator_request_bytes = 0u;
    /// Payload allocation bound used when reserving memory.
    std::uint64_t payload_allocation_requirement_bytes = 0u;
    /// Size of the stable_state control object.
    std::uint64_t control_object_bytes = 0u;
    /// Bytes requested from the allocator  for stable_state.
    std::uint64_t control_allocator_request_bytes = 0u;
    /// Control allocation bound used when reserving memory
    std::uint64_t control_allocation_requirement_bytes = 0u;
    /// Combined payload and control allocation bounds, checkej for overflow.
    std::uint64_t total_allocation_requirement_bytes = 0u;
};

/** Controls whether materialization may decode and repack logical ranks. */
enum class packed_materialization_policy : std::uint8_t {
    allow_rank_repack,
    require_native_copy,
};

/** Raised when native copying is required but unavailable for the requested region. */
class native_packed_materialization_unavailable final : public std::runtime_error {
  public:
    native_packed_materialization_unavailable()
        : std::runtime_error(
              "native packed materialization is unavailable for this sequence facade") {}
};

/** Computes payload and owner allocation bounds, rejecting unsupported widths or size overflow. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] packed_sequence_physical_plan
plan_packed_sequence_allocation(sequence_size_t logical_length,
                                std::uint8_t requested_width);

/** Returns the memory reservation needed for a packed copy and its owner. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint64_t packed_sequence_admission_bytes(sequence_size_t logical_length,
                                                            std::uint8_t requested_width);

template <sequence_alphabet p_alphabet>
[[nodiscard]] admitted_packed_sequence<p_alphabet>
materialize_packed_sequence(lazy_sequence_view<p_alphabet> view,
                            sequence_size_t from,
                            sequence_size_t to,
                            std::uint8_t requested_width,
                            bioseq::parallelism::memory_permit&& permit,
                            packed_materialization_policy policy =
                                packed_materialization_policy::allow_rank_repack,
                            bioseq::parallelism::memory_footprint_probe footprint_probe =
                                bioseq::parallelism::live_process_footprint_probe);

/**
 * Owns an aligned packed sequence and any memory reservation still held for it.
 *
 * The control object stays at a fixed address when this owner is moved, so
 * previously  returned views remain valid until the owning object is destroyed.
 * Views borrow storage and must not outlive it.
 */
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

    [[nodiscard]] std::uint64_t test_payload_allocation_bytes() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("payload-allocation query on moved packed region");
        }
        return state_->units_.allocation_bytes();
    }

    [[nodiscard]] std::uint64_t test_control_allocation_bytes() const noexcept {
        if (state_ == nullptr) {
            abort_lazy_sequence_access("control-allocation query on moved packed region");
        }
        return state_->allocation_bytes();
    }
#endif

  private:
    struct stable_state {
        bioseq::parallelism::memory_permit permit_;
        lazy_sequence_detail::aligned_packed_storage units_;
        descriptor_type descriptor_;
        lazy_sequence_detail::resident_view_ops_context<p_alphabet> view_context_;

        // Allocate and touch the full control-object allocation before constructing
        // its members, so the memory measurement includes its backing pages.
        static void* operator new(std::size_t bytes) {
            if (bytes != sizeof(stable_state)) {
                throw std::length_error("packed control allocation has an unexpected extent");
            }
            const auto plan = bioseq::common::heap_allocation_detail::plan_heap_allocation(bytes);
            std::size_t allocation_bytes = 0u;
            void* allocation = bioseq::common::heap_allocation_detail::allocate_and_touch(
                plan, allocation_bytes);
            if (allocation == nullptr) {
                throw std::bad_alloc{};
            }
            return allocation;
        }

        static void operator delete(void* allocation) noexcept {
            std::free(allocation);
        }

        /** Releases the reservation after freeing both the payload and control object. */
        static void operator delete(stable_state* allocation,
                                    std::destroying_delete_t) noexcept {
            auto permit = std::move(allocation->permit_);
            std::destroy_at(allocation);
            std::free(allocation);
            permit.release();
        }

        stable_state(std::size_t allocation_bytes,
                     std::size_t highway_request_bound,
                     const bioseq::common::heap_allocation_detail::heap_allocation_plan& payload_plan,
                     descriptor_type descriptor)
            : units_(lazy_sequence_detail::aligned_packed_storage::allocate(
                  allocation_bytes, highway_request_bound, payload_plan)),
              descriptor_(descriptor), view_context_{std::addressof(descriptor_)} {
            descriptor_.units = units_.data();
        }

        [[nodiscard]] std::size_t allocation_bytes() noexcept {
            return bioseq::common::heap_allocation_detail::allocator_reported_bytes(this);
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
                                bioseq::parallelism::memory_permit&&,
                                packed_materialization_policy,
                                bioseq::parallelism::memory_footprint_probe);

    template <sequence_alphabet p_other_alphabet>
    friend std::uint64_t packed_sequence_admission_bytes(sequence_size_t, std::uint8_t);

    template <sequence_alphabet p_other_alphabet>
    friend packed_sequence_physical_plan
    plan_packed_sequence_allocation(sequence_size_t, std::uint8_t);

    std::unique_ptr<stable_state> state_;
};

template <sequence_alphabet p_alphabet>
[[nodiscard]] packed_sequence_physical_plan
plan_packed_sequence_allocation(sequence_size_t logical_length,
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
    const std::size_t payload_request = lazy_sequence_detail::packed_allocator_request_bound(
        static_cast<std::size_t>(*allocation_bytes));
    const auto payload_plan = bioseq::common::heap_allocation_detail::plan_heap_allocation(payload_request);
    const auto control_plan = bioseq::common::heap_allocation_detail::plan_heap_allocation(
        sizeof(typename owner_type::stable_state));
    const auto total_requirement = bioseq::common::checked_add(
        static_cast<std::uint64_t>(payload_plan.allocation_requirement_bytes),
        static_cast<std::uint64_t>(control_plan.allocation_requirement_bytes));
    if (!total_requirement.has_value()) {
        throw std::overflow_error("packed materialization admission overflow");
    }
    return {
        .logical_payload_bytes = *allocation_bytes,
        .payload_highway_request_bound_bytes = payload_request,
        .payload_allocator_request_bytes = payload_plan.allocator_request_bytes,
        .payload_allocation_requirement_bytes = payload_plan.allocation_requirement_bytes,
        .control_object_bytes = sizeof(typename owner_type::stable_state),
        .control_allocator_request_bytes = control_plan.allocator_request_bytes,
        .control_allocation_requirement_bytes = control_plan.allocation_requirement_bytes,
        .total_allocation_requirement_bytes = *total_requirement,
    };
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint64_t packed_sequence_admission_bytes(sequence_size_t logical_length,
                                                            std::uint8_t requested_width) {
    return plan_packed_sequence_allocation<p_alphabet>(logical_length, requested_width)
        .total_allocation_requirement_bytes;
}

/**
 * Copies [from, to) into aligned packed storage using an existing memory permit.
 *
 * The permit must cover the payload and control allocations. Memory is measured
 * befare source reads begin. Complete sequences first  try native packed copying.
 * Other regions or layouts are packed from ranks when policy allows it.
 *
 * The owner keeps storage and any remaining reservation alive. Invalid inputs,
 * insufficient permits and allocation, or packing failures raise exceptions.
 * Requiring native copying also raises if that path is unavailable.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] admitted_packed_sequence<p_alphabet>
materialize_packed_sequence(lazy_sequence_view<p_alphabet> view,
                            sequence_size_t from,
                            sequence_size_t to,
                            std::uint8_t requested_width,
                            bioseq::parallelism::memory_permit&& permit,
                            packed_materialization_policy policy,
                            bioseq::parallelism::memory_footprint_probe footprint_probe) {
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
    const packed_sequence_physical_plan physical_plan =
        plan_packed_sequence_allocation<p_alphabet>(length, requested_width);
    const std::uint64_t admitted_bytes = physical_plan.total_allocation_requirement_bytes;
    if (!permit || permit.bytes() < admitted_bytes) {
        throw std::length_error("packed materialization permit is smaller than retained storage");
    }
    const auto payload_heap_plan = bioseq::common::heap_allocation_detail::heap_allocation_plan{
        .allocator_request_bytes = static_cast<std::size_t>(
            physical_plan.payload_allocator_request_bytes),
        .allocation_requirement_bytes = static_cast<std::size_t>(
            physical_plan.payload_allocation_requirement_bytes),
    };

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
    // Keep the caller's permit through allocation and memory measurement
    // Transfer any remaining reservation to the owner after the checks pass.
    std::unique_ptr<typename owner_type::stable_state> state;
    auto& admission = *permit.controller();
    const auto materialization = admission.materialize(permit,
        [&state,
         &permit,
         allocation_bytes,
         highway_request_bound = physical_plan.payload_highway_request_bound_bytes,
         payload_heap_plan,
         descriptor] {
            state = std::make_unique<typename owner_type::stable_state>(
                allocation_bytes,
                static_cast<std::size_t>(highway_request_bound),
                payload_heap_plan,
                descriptor);
            const auto actual = bioseq::common::checked_add(
                static_cast<std::uint64_t>(state->units_.allocation_bytes()),
                static_cast<std::uint64_t>(state->allocation_bytes()));
            if (!actual || *actual > permit.bytes()) {
                throw std::length_error("packed physical allocation exceeded its admitted bound");
            }
            permit.shrink_to(*actual);
            return bioseq::parallelism::memory_materialization_snapshot{
                .actual_allocation_bytes = *actual,
                .fully_touched_bytes = *actual,
                .is_host_visible = true,
                .is_host_coherent = true,
                .is_process_footprint_reflected = true,
            };
        }, footprint_probe);
    const auto retained_bytes = bioseq::common::checked_add(
        static_cast<std::uint64_t>(state->units_.allocation_bytes()),
        static_cast<std::uint64_t>(state->allocation_bytes()));
    if (!retained_bytes.has_value() || state->permit_ ||
        materialization.admitted_bytes != *retained_bytes ||
        materialization.allocation_snapshot.actual_allocation_bytes != *retained_bytes ||
        materialization.allocation_snapshot.fully_touched_bytes != *retained_bytes ||
        materialization.is_permit_released == static_cast<bool>(permit) ||
        (permit && permit.bytes() != *retained_bytes)) {
        throw std::logic_error("packed materialization physical ownership proof is inconsistent");
    }
    state->permit_ = std::move(permit);

    // Read the source after the allocation transaction ends. Lazy reads can
    // allocate memory themselves  and must not nest that transaction.
    const bool complete_range = from == 0u && to == view.bp_size();
    bool native_copied = complete_range &&
                         view.try_copy_packed_sequence(
                             std::span<std::uint8_t>{state->units_.data(), allocation_bytes},
                             static_cast<std::size_t>(unit_count),
                             requested_width);
    if (!native_copied && complete_range) {
        const auto copied = view.try_copy_packed_window(
            std::span<std::uint8_t>{state->units_.data(), allocation_bytes},
            from,
            to,
            requested_width);
        native_copied = copied.has_value() && copied->units == state->units_.data() &&
                        copied->unit_count == unit_count &&
                        copied->logical_length == length &&
                        copied->leading_symbol_offset == 0u &&
                        copied->trailing_padding == trailing &&
                        copied->storage_unit_width == requested_width &&
                        copied->bits_per_symbol == k_bits_per_symbol &&
                        copied->bit_order == bit_order_type::least_significant_symbol_first;
    }
    if (!native_copied && policy == packed_materialization_policy::require_native_copy) {
        throw native_packed_materialization_unavailable{};
    }
    if (!native_copied) {
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
                                      *retained_bytes);
    return owner_type{std::move(state)};
}

static_assert(std::is_move_constructible_v<admitted_packed_sequence<seq::dna4>>);
static_assert(!std::is_move_assignable_v<admitted_packed_sequence<seq::dna4>>);
static_assert(!std::is_copy_constructible_v<admitted_packed_sequence<seq::dna4>>);

} // namespace bioseq::storage
