/**
 * @file representation.cppm
 * @brief Exact minimizer record layouts and query bounds.
 */
module;

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.highway_minimizer_contract:representation;

import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
import bioseq.common.logging;
#endif
import bioseq.parallelism.memory_admission;
import bioseq.sequences;
import bioseq.common.heap_allocation;
import bioseq.storage.size;
import bioseq.sequences.kmers.minimizer_layout_geometry;
import bioseq.pipeline.runtime_contract;
export import bioseq.sequences.kmers.minimizer_physical_records;

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
namespace bioseq::minimizers::physical_batch_owner_detail {

/** Flush one shared-owner allocation failure without replacing its exception. */
inline void emit_admitted_shared_owner_failure(
    const char* site,
    std::uint64_t payload_capacity_bytes,
    std::uint64_t request_bytes,
    std::uint64_t allocation_requirement_bytes,
    std::uint64_t permit_bytes,
    std::uint64_t admitted_total_bytes,
    std::uint64_t actual_bytes,
    bool allocation_is_null,
    const bioseq::parallelism::memory_admission_failure* failure = nullptr) noexcept {
    const auto reason = failure != nullptr && failure->has_failure()
                            ? bioseq::parallelism::memory_admission_failure_reason_name(
                                  failure->reason)
                            : std::string_view{"none"};
    try {
        bioseq_log_progress<logging_level::warning, logging_target::file>(
            "[bioseq.admitted_shared_owner.failure] site={} payload_capacity_bytes={} "
            "request_bytes={} allocation_requirement_bytes={} permit_bytes={} "
            "admitted_total_bytes={} actual_bytes={} allocation_is_null={} "
            "admission_reason={} requested_growth_bytes={} reserved_bytes={} "
            "budget_ceiling_bytes={} allocatable_after_reservations_bytes={} "
            "process_footprint_bytes={} system_available_bytes={} system_total_bytes={} "
            "process_soft_cap_bytes={}",
            site,
            static_cast<unsigned long long>(payload_capacity_bytes),
            static_cast<unsigned long long>(request_bytes),
            static_cast<unsigned long long>(allocation_requirement_bytes),
            static_cast<unsigned long long>(permit_bytes),
            static_cast<unsigned long long>(admitted_total_bytes),
            static_cast<unsigned long long>(actual_bytes),
            allocation_is_null,
            reason,
            static_cast<unsigned long long>(failure != nullptr ? failure->requested_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr ? failure->reserved_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->budget_ceiling_bytes
                                                : 0u),
            static_cast<unsigned long long>(
                failure != nullptr ? failure->allocatable_after_reservations_bytes : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->process_footprint_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->system_available_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->system_total_bytes
                                                : 0u),
            static_cast<unsigned long long>(failure != nullptr
                                                ? failure->process_soft_cap_bytes
                                                : 0u));
    } catch (...) {
        // Diagnostics must not replace the allocator's existing exception.
    }
}

} // namespace bioseq::minimizers::physical_batch_owner_detail
#endif

export namespace bioseq::minimizers {

template <std::size_t p_max_sequence_log2, std::size_t p_kmer_length, bool p_is_128_bit>
struct minimizer_representation;

namespace minimizer_physical_record_detail {

template <class>
struct record_coordinates {
    static constexpr bool is_representation = false;
};

template <std::size_t p_position_log2,
          std::size_t p_physical_kmer_size,
          bool p_is_128_bit>
struct record_coordinates<minimizer_representation<
    p_position_log2,
    p_physical_kmer_size,
    p_is_128_bit>> {
    static constexpr bool is_representation = true;
    static constexpr bool is_128_bit = p_is_128_bit;
    static constexpr std::size_t position_log2 = p_position_log2;
    static constexpr std::size_t physical_kmer_size = p_physical_kmer_size;
};

/** Map only an exact canonical representation specialization into the generated table. */
template <class p_record>
[[nodiscard]] consteval minimizer_physical_record_id record_id() noexcept {
    using record_type = std::remove_cvref_t<p_record>;
    using coordinates = record_coordinates<record_type>;
    if constexpr (!coordinates::is_representation) {
        return minimizer_physical_record_id::invalid;
    } else {
        for (const auto& descriptor : registered_minimizer_physical_records()) {
            if (descriptor.position_log2 == coordinates::position_log2 &&
                descriptor.physical_kmer_size == coordinates::physical_kmer_size &&
                (descriptor.storage_bits == 128u) == coordinates::is_128_bit &&
                descriptor.storage_bits == sizeof(record_type) * 8u &&
                descriptor.record_bytes == sizeof(record_type)) {
                return descriptor.record_id;
            }
        }
        return minimizer_physical_record_id::invalid;
    }
}

/** Whether the selected generated registry contains a 32-bit physical record. */
[[nodiscard]] consteval bool has_32_bit_record() noexcept {
    for (const auto& descriptor : registered_minimizer_physical_records()) {
        if (descriptor.record_bytes == sizeof(std::uint32_t)) {
            return true;
        }
    }
    return false;
}

inline constexpr bool k_has_32_bit_record = has_32_bit_record();

} // namespace minimizer_physical_record_detail

/** Exact canonical record specialization present in the generated physical table. */
template <class p_record>
concept generated_minimizer_physical_record =
    std::same_as<p_record, std::remove_cvref_t<p_record>> &&
    (minimizer_physical_record_detail::record_id<p_record>() !=
     minimizer_physical_record_id::invalid);

/** Generated record supported by the existing 32/64/128-bit decoded batch view. */
template <class p_record>
concept generated_minimizer_physical_batch_record =
    generated_minimizer_physical_record<p_record> &&
    (sizeof(p_record) == 8u || sizeof(p_record) == 16u ||
     (minimizer_physical_record_detail::k_has_32_bit_record &&
      sizeof(p_record) == sizeof(std::uint32_t)));

/** Physical format family, independent of the requested biological k/w. */
enum class minimizer_physical_layout_family : std::uint8_t {
    fixed_u32_hash_legacy_k15,
    minimap2_reference_exact_k,
    minimap2_scalar_reference_exact_k,
};

/** Immutable value selection consumed before any record type is instantiated. */
struct minimizer_physical_layout_selection {
    minimizer_physical_layout_family family;
    minimizer_physical_record_id record_id;
    ::bioseq::simd::minimizer_algorithm requested_algorithm;
    ::bioseq::simd::minimizer_algorithm effective_algorithm;
    std::uint16_t alphabet_id;
    std::uint8_t record_position_log2;
    std::uint8_t physical_kmer_size;
    minimizer_scalar_width hash_width;
    std::uint8_t packed_storage_bits;
    std::uint16_t runtime_kmer_size;
    std::uint16_t runtime_window_size;

    [[nodiscard]] bool operator==(const minimizer_physical_layout_selection&) const = default;
};

/** Width-independent fields decoded from one registered physical record. */
struct minimizer_physical_record_value {
    std::uint64_t hash = 0u;
    std::uint64_t position_strand = 0u;

    /** Canonical hash carried by the source record. */
    [[nodiscard]] constexpr std::uint64_t kmer() const noexcept {
        return hash;
    }

    /** Packed `(last_position << 1) | strand` carried by the source record. */
    [[nodiscard]] constexpr std::uint64_t pos_strand_encoded() const noexcept {
        return position_strand;
    }
};

/**
 * Non-owning, record-layout-authenticated view used below the generated visitor.
 *
 * This is an execution adapter, not a second record registry: construction is
 * possible only from a concrete record selected by the generated registry. The
 * selected descriptor authenticates one homogeneous 32-, 64-, or 128-bit record ABI;
 * bulk decoding branches on that ABI once and constructs values field-wise.
 */
class minimizer_physical_batch_view {
  private:
    const void* data_identity_ = nullptr;
    std::size_t size_ = 0u;
    minimizer_physical_record_id record_id_ = minimizer_physical_record_id::invalid;
    std::uint8_t record_bytes_ = 0u;
    std::uint8_t hash_word_offset_ = 0u;
    std::uint8_t hash_shift_ = 0u;
    std::uint64_t position_strand_mask_ = 0u;

    template <generated_minimizer_physical_record p_record>
    [[nodiscard]] static constexpr bool record_descriptor_matches_(
        const minimizer_physical_record_descriptor& descriptor) noexcept {
        using coordinates = minimizer_physical_record_detail::record_coordinates<
            std::remove_cvref_t<p_record>>;
        return descriptor.record_id ==
                   minimizer_physical_record_detail::record_id<p_record>() &&
               descriptor.position_log2 == coordinates::position_log2 &&
               descriptor.physical_kmer_size ==
                   coordinates::physical_kmer_size &&
               descriptor.storage_bits == sizeof(p_record) * 8u &&
               descriptor.record_bytes == sizeof(p_record);
    }

    template <generated_minimizer_physical_record p_record>
    [[nodiscard]] static constexpr minimizer_physical_record_id record_id_for_() noexcept {
        return minimizer_physical_record_detail::record_id<p_record>();
    }

    [[nodiscard]] static constexpr std::uint64_t
    position_strand_mask(const std::uint8_t position_log2) noexcept {
        return position_log2 >= 63u
                   ? (std::numeric_limits<std::uint64_t>::max)()
                   : (std::uint64_t{1u} << (position_log2 + 1u)) - 1u;
    }

    constexpr minimizer_physical_batch_view(const void* data_identity,
                                            const std::size_t size,
                                            const minimizer_physical_record_id record_id,
                                            const std::uint8_t record_bytes,
                                            const std::uint8_t position_log2) noexcept
        : data_identity_(data_identity), size_(size), record_id_(record_id),
          record_bytes_(record_bytes),
          hash_word_offset_(record_bytes == 16u ? 8u : 0u),
          hash_shift_(record_bytes == 16u ? 8u
                                         : static_cast<std::uint8_t>(position_log2 + 1u)),
          position_strand_mask_(position_strand_mask(position_log2)) {}

    friend class minimizer_physical_batch_owner;

  public:
    constexpr minimizer_physical_batch_view() noexcept = default;

    /** Convert a concrete registered-record span into an immediate borrowed view. */
    template <generated_minimizer_physical_batch_record p_record>
    minimizer_physical_batch_view(const std::span<const p_record> records)
        : minimizer_physical_batch_view(from_span(records)) {}

    /** Adapt a named contiguous record owner without permitting a temporary owner. */
    template <std::ranges::contiguous_range p_range>
        requires generated_minimizer_physical_batch_record<
            std::ranges::range_value_t<p_range>>
    minimizer_physical_batch_view(p_range& records)
        : minimizer_physical_batch_view(from_span(
              std::span<const std::ranges::range_value_t<p_range>>{records})) {}

    /** Bind an immediate borrowed span of one registered concrete record. */
    template <generated_minimizer_physical_batch_record p_record>
    [[nodiscard]] static minimizer_physical_batch_view from_span(
        const std::span<const p_record> records) {
        constexpr minimizer_physical_record_id record_id = record_id_for_<p_record>();
        static_assert(record_id != minimizer_physical_record_id::invalid,
                      "minimizer batch view requires a generated physical record");
        static_assert(std::is_trivially_copyable_v<p_record> &&
                          std::is_standard_layout_v<p_record>,
                      "minimizer batch view requires the generated value-record ABI");
        static_assert(sizeof(p_record) == 8u || sizeof(p_record) == 16u ||
                          (minimizer_physical_record_detail::k_has_32_bit_record &&
                           sizeof(p_record) == sizeof(std::uint32_t)),
                      "minimizer batch view requires the registered 32/64/128-bit record ABI");
        return minimizer_physical_batch_view{
            records.data(),
            records.size(),
            record_id,
            static_cast<std::uint8_t>(sizeof(p_record)),
            static_cast<std::uint8_t>(
                minimizer_physical_record_detail::record_coordinates<
                    std::remove_cvref_t<p_record>>::position_log2)};
    }

    /** Return the authenticated physical record tag. */
    [[nodiscard]] constexpr minimizer_physical_record_id record_id() const noexcept {
        return record_id_;
    }

    /** Return the number of physical records in the view. */
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    /** Return whether no records are present. */
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0u;
    }

    /** Return the concrete vector/span address used for lifetime checks. */
    [[nodiscard]] constexpr const void* data_owner_address() const noexcept {
        return data_identity_;
    }

    /** Decode one prevalidated record without a registry switch or indirect call. */
    [[nodiscard, gnu::always_inline]] minimizer_physical_record_value
    at_unchecked(const std::size_t ordinal) const noexcept {
        const auto* const record = static_cast<const std::byte*>(data_identity_) +
                                   ordinal * record_bytes_;
        if constexpr (minimizer_physical_record_detail::k_has_32_bit_record) {
            if (record_bytes_ == sizeof(std::uint32_t)) {
                std::uint32_t word = 0u;
                std::memcpy(std::addressof(word), record, sizeof(word));
                const std::uint64_t widened_word = word;
                return {
                    .hash = widened_word >> hash_shift_,
                    .position_strand = widened_word & position_strand_mask_,
                };
            }
        }
        std::uint64_t position_word = 0u;
        std::memcpy(std::addressof(position_word), record, sizeof(position_word));
        std::uint64_t hash_word = position_word;
        if (hash_word_offset_ != 0u) {
            std::memcpy(std::addressof(hash_word),
                        record + hash_word_offset_,
                        sizeof(hash_word));
        }
        return {
            .hash = hash_word >> hash_shift_,
            .position_strand = position_word & position_strand_mask_,
        };
    }

    /** Decode only packed position/strand; it is always the first native word. */
    [[nodiscard, gnu::always_inline]] std::uint64_t
    position_strand_at_unchecked(const std::size_t ordinal) const noexcept {
        const auto* const record = static_cast<const std::byte*>(data_identity_) +
                                   ordinal * record_bytes_;
        if constexpr (minimizer_physical_record_detail::k_has_32_bit_record) {
            if (record_bytes_ == sizeof(std::uint32_t)) {
                std::uint32_t word = 0u;
                std::memcpy(std::addressof(word), record, sizeof(word));
                return static_cast<std::uint64_t>(word) & position_strand_mask_;
            }
        }
        std::uint64_t position_word = 0u;
        std::memcpy(std::addressof(position_word), record, sizeof(position_word));
        return position_word & position_strand_mask_;
    }

    /** Decode a whole homogeneous owner with one native-stride loop. */
    template <class p_visitor>
    [[gnu::always_inline]] void for_each_decoded(p_visitor&& visitor) const {
        const auto* record = static_cast<const std::byte*>(data_identity_);
        if (record_bytes_ == sizeof(std::uint64_t)) {
            for (std::size_t ordinal = 0u; ordinal < size_;
                 ++ordinal, record += sizeof(std::uint64_t)) {
                std::uint64_t word = 0u;
                std::memcpy(std::addressof(word), record, sizeof(word));
                visitor(ordinal,
                        minimizer_physical_record_value{
                            .hash = word >> hash_shift_,
                            .position_strand = word & position_strand_mask_,
                        });
            }
            return;
        }
        if constexpr (minimizer_physical_record_detail::k_has_32_bit_record) {
            if (record_bytes_ == sizeof(std::uint32_t)) {
                for (std::size_t ordinal = 0u; ordinal < size_;
                     ++ordinal, record += sizeof(std::uint32_t)) {
                    std::uint32_t word = 0u;
                    std::memcpy(std::addressof(word), record, sizeof(word));
                    const std::uint64_t widened_word = word;
                    visitor(ordinal,
                            minimizer_physical_record_value{
                                .hash = widened_word >> hash_shift_,
                                .position_strand = widened_word & position_strand_mask_,
                            });
                }
                return;
            }
        }
        for (std::size_t ordinal = 0u; ordinal < size_; ++ordinal, record += 16u) {
            std::uint64_t position_word = 0u;
            std::uint64_t hash_word = 0u;
            std::memcpy(std::addressof(position_word), record, sizeof(position_word));
            std::memcpy(std::addressof(hash_word), record + 8u, sizeof(hash_word));
            visitor(ordinal,
                    minimizer_physical_record_value{
                        .hash = hash_word >> 8u,
                        .position_strand = position_word & position_strand_mask_,
                    });
        }
    }

    /** Decode a homogeneous owner in reverse order with one native-stride branch. */
    template <class p_visitor>
    [[gnu::always_inline]] void for_each_decoded_reverse(p_visitor&& visitor) const {
        if (size_ == 0u) {
            return;
        }
        const auto* const begin = static_cast<const std::byte*>(data_identity_);
        if (record_bytes_ == sizeof(std::uint64_t)) {
            const auto* record = begin + size_ * sizeof(std::uint64_t);
            for (std::size_t ordinal = size_; ordinal-- > 0u;) {
                record -= sizeof(std::uint64_t);
                std::uint64_t word = 0u;
                std::memcpy(std::addressof(word), record, sizeof(word));
                visitor(ordinal,
                        minimizer_physical_record_value{
                            .hash = word >> hash_shift_,
                            .position_strand = word & position_strand_mask_,
                        });
            }
            return;
        }
        if constexpr (minimizer_physical_record_detail::k_has_32_bit_record) {
            if (record_bytes_ == sizeof(std::uint32_t)) {
                const auto* record = begin + size_ * sizeof(std::uint32_t);
                for (std::size_t ordinal = size_; ordinal-- > 0u;) {
                    record -= sizeof(std::uint32_t);
                    std::uint32_t word = 0u;
                    std::memcpy(std::addressof(word), record, sizeof(word));
                    const std::uint64_t widened_word = word;
                    visitor(ordinal,
                            minimizer_physical_record_value{
                                .hash = widened_word >> hash_shift_,
                                .position_strand = widened_word & position_strand_mask_,
                            });
                }
                return;
            }
        }
        const auto* record = begin + size_ * 16u;
        for (std::size_t ordinal = size_; ordinal-- > 0u;) {
            record -= 16u;
            std::uint64_t position_word = 0u;
            std::uint64_t hash_word = 0u;
            std::memcpy(std::addressof(position_word), record, sizeof(position_word));
            std::memcpy(std::addressof(hash_word), record + 8u, sizeof(hash_word));
            visitor(ordinal,
                    minimizer_physical_record_value{
                        .hash = hash_word >> 8u,
                        .position_strand = position_word & position_strand_mask_,
                    });
        }
    }

    /** Decode one checked record for cold diagnostics and focused tests. */
    [[nodiscard]] minimizer_physical_record_value at(std::size_t ordinal) const;
};

namespace physical_batch_owner_detail {

struct allocation_transaction {
    bioseq::parallelism::memory_permit* permit = nullptr;
    std::uint64_t payload_capacity_bytes = 0u;
    std::uint64_t owner_allocation_bytes = 0u;
    bool committed = false;
};

inline thread_local allocation_transaction* active_transaction = nullptr;

class transaction_scope {
  public:
    transaction_scope(bioseq::parallelism::memory_permit& permit,
                      const std::uint64_t payload_capacity_bytes)
        : transaction_{
              .permit = std::addressof(permit),
              .payload_capacity_bytes = payload_capacity_bytes,
          } {
        if (active_transaction != nullptr || permit.bytes() != payload_capacity_bytes) {
            throw std::logic_error{"minimizer shared-owner admission transaction is invalid"};
        }
        active_transaction = std::addressof(transaction_);
    }

    transaction_scope(const transaction_scope&) = delete;
    transaction_scope& operator=(const transaction_scope&) = delete;

    ~transaction_scope() {
        active_transaction = nullptr;
        if (!transaction_.committed && transaction_.permit != nullptr) {
            transaction_.permit->shrink_to(transaction_.payload_capacity_bytes);
        }
    }

    [[nodiscard]] std::uint64_t owner_allocation_bytes() const noexcept {
        return transaction_.owner_allocation_bytes;
    }

    void commit() noexcept {
        transaction_.committed = true;
    }

  private:
    allocation_transaction transaction_{};
};

/** Stateless allocator retained safely by the shared control block. */
template <class p_value>
class admitted_shared_owner_allocator {
  public:
    using value_type = p_value;
    using is_always_equal = std::true_type;

    admitted_shared_owner_allocator() noexcept = default;

    template <class p_other>
    admitted_shared_owner_allocator(const admitted_shared_owner_allocator<p_other>&) noexcept {}

    [[nodiscard]] p_value* allocate(const std::size_t count) {
        static_assert(alignof(p_value) <= alignof(std::max_align_t));
        static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
        allocation_transaction* const transaction = active_transaction;
        if (transaction == nullptr || transaction->permit == nullptr || count == 0u ||
            transaction->owner_allocation_bytes != 0u ||
            count > (std::numeric_limits<std::size_t>::max)() / sizeof(p_value)) {
            throw std::bad_array_new_length{};
        }
        const std::size_t request_bytes = count * sizeof(p_value);
        const auto plan =
            bioseq::common::heap_allocation_detail::plan_heap_allocation(request_bytes);
        const auto admitted_total = bioseq::common::checked_add(
            transaction->payload_capacity_bytes,
            static_cast<std::uint64_t>(plan.allocation_requirement_bytes));
        if (!admitted_total) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            emit_admitted_shared_owner_failure(
                "admitted_total_overflow",
                transaction->payload_capacity_bytes,
                static_cast<std::uint64_t>(request_bytes),
                static_cast<std::uint64_t>(plan.allocation_requirement_bytes),
                transaction->permit->bytes(),
                0u,
                0u,
                false);
#endif
            throw std::bad_alloc{};
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        bioseq::parallelism::memory_admission_failure admission_failure{};
        if (!transaction->permit->grow_to(*admitted_total,
                                          std::addressof(admission_failure))) {
            emit_admitted_shared_owner_failure(
                "permit_growth_denied",
                transaction->payload_capacity_bytes,
                static_cast<std::uint64_t>(request_bytes),
                static_cast<std::uint64_t>(plan.allocation_requirement_bytes),
                transaction->permit->bytes(),
                *admitted_total,
                0u,
                false,
                std::addressof(admission_failure));
            throw std::bad_alloc{};
        }
#else
        if (!transaction->permit->grow_to(*admitted_total)) {
            throw std::bad_alloc{};
        }
#endif
        std::size_t actual_bytes = 0u;
        void* allocation = bioseq::common::heap_allocation_detail::allocate_and_touch(
            plan, actual_bytes);
        if (allocation == nullptr || actual_bytes == 0u) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            emit_admitted_shared_owner_failure(
                "physical_allocation_or_extent_invalid",
                transaction->payload_capacity_bytes,
                static_cast<std::uint64_t>(request_bytes),
                static_cast<std::uint64_t>(plan.allocation_requirement_bytes),
                transaction->permit->bytes(),
                *admitted_total,
                static_cast<std::uint64_t>(actual_bytes),
                allocation == nullptr);
#endif
            transaction->permit->shrink_to(transaction->payload_capacity_bytes);
            throw std::bad_alloc{};
        }
        const auto retained_total = bioseq::common::checked_add(
            transaction->payload_capacity_bytes,
            static_cast<std::uint64_t>(actual_bytes));
        if (!retained_total) {
            std::free(allocation);
            transaction->permit->shrink_to(transaction->payload_capacity_bytes);
            throw std::bad_array_new_length{};
        }
        transaction->permit->shrink_to(*retained_total);
        transaction->owner_allocation_bytes = actual_bytes;
        return static_cast<p_value*>(allocation);
    }

    void deallocate(p_value* allocation, std::size_t) noexcept {
        std::free(const_cast<void*>(static_cast<const void*>(allocation)));
    }

    template <class p_other>
    [[nodiscard]] constexpr bool
    operator==(const admitted_shared_owner_allocator<p_other>&) const noexcept {
        return true;
    }
};

} // namespace physical_batch_owner_detail

/**
 * Real shared owner for a vector of one generated physical minimizer record.
 *
 * The erased pointer always shares the concrete vector's control block. No
 * no-op deleter or borrowed-owner substitution is accepted. Consumers recover
 * a typed shared owner only after the generated visitor selects the same tag.
 * The factory reconciles payload plus allocator-reported control/vector-object
 * capacity into the caller's adjacent permit before publication.
 */
class minimizer_physical_batch_owner {
  private:
    minimizer_physical_layout_selection layout_{};
    std::shared_ptr<const void> vector_owner_{};
    const void* data_identity_ = nullptr;
    std::size_t size_ = 0u;
    std::size_t capacity_ = 0u;
    std::uint64_t payload_capacity_bytes_ = 0u;
    std::uint64_t owner_allocation_bytes_ = 0u;
    std::uint64_t capacity_bytes_ = 0u;
    std::uint8_t record_bytes_ = 0u;

    template <generated_minimizer_physical_record p_record>
    [[nodiscard]] static bool layout_matches_record_(
        const minimizer_physical_layout_selection& layout) noexcept {
        const auto* descriptor = find_minimizer_physical_record(layout.record_id);
        return descriptor != nullptr &&
               minimizer_physical_batch_view::record_descriptor_matches_<p_record>(*descriptor) &&
               layout.record_position_log2 == descriptor->position_log2 &&
               layout.physical_kmer_size == descriptor->physical_kmer_size;
    }

    /** Erase a concrete, immutable vector while preserving its real shared owner. */
    template <generated_minimizer_physical_record p_record>
    [[nodiscard]] static minimizer_physical_batch_owner from_shared_host(
        const minimizer_physical_layout_selection& layout,
        std::shared_ptr<const std::vector<p_record>> owner,
        const std::uint64_t owner_allocation_bytes) {
        if (!owner || owner_allocation_bytes == 0u ||
            !layout_matches_record_<p_record>(layout)) {
            throw std::invalid_argument{
                "minimizer physical owner disagrees with its generated record"};
        }
        const std::uint64_t payload_capacity_bytes =
            owner->capacity() > (std::numeric_limits<std::uint64_t>::max)() / sizeof(p_record)
                ? (std::numeric_limits<std::uint64_t>::max)()
                : static_cast<std::uint64_t>(owner->capacity()) * sizeof(p_record);
        const std::uint64_t capacity_bytes = bioseq::common::saturating_add(
            payload_capacity_bytes, owner_allocation_bytes);
        if (payload_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            capacity_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            throw std::length_error{"minimizer physical owner capacity overflows"};
        }
        minimizer_physical_batch_owner result;
        result.layout_ = layout;
        result.size_ = owner->size();
        result.capacity_ = owner->capacity();
        result.payload_capacity_bytes_ = payload_capacity_bytes;
        result.owner_allocation_bytes_ = owner_allocation_bytes;
        result.capacity_bytes_ = capacity_bytes;
        result.record_bytes_ = static_cast<std::uint8_t>(sizeof(p_record));
        result.data_identity_ = owner->data();
        result.vector_owner_ = std::static_pointer_cast<const void>(std::move(owner));
        return result;
    }

    void move_from_(minimizer_physical_batch_owner& other) noexcept {
        layout_ = std::exchange(other.layout_, {});
        vector_owner_ = std::move(other.vector_owner_);
        data_identity_ = std::exchange(other.data_identity_, nullptr);
        size_ = std::exchange(other.size_, 0u);
        capacity_ = std::exchange(other.capacity_, 0u);
        payload_capacity_bytes_ = std::exchange(other.payload_capacity_bytes_, 0u);
        owner_allocation_bytes_ = std::exchange(other.owner_allocation_bytes_, 0u);
        capacity_bytes_ = std::exchange(other.capacity_bytes_, 0u);
        record_bytes_ = std::exchange(other.record_bytes_, 0u);
    }

  public:
    minimizer_physical_batch_owner() noexcept = default;
    minimizer_physical_batch_owner(const minimizer_physical_batch_owner&) = delete;
    minimizer_physical_batch_owner& operator=(const minimizer_physical_batch_owner&) = delete;

    minimizer_physical_batch_owner(minimizer_physical_batch_owner&& other) noexcept {
        move_from_(other);
    }

    minimizer_physical_batch_owner& operator=(minimizer_physical_batch_owner&& other) noexcept {
        if (this != std::addressof(other)) {
            reset();
            move_from_(other);
        }
        return *this;
    }

    /** Move one concrete vector into a new real shared owner. */
    template <generated_minimizer_physical_record p_record>
    [[nodiscard]] static minimizer_physical_batch_owner from_owned_host(
        const minimizer_physical_layout_selection& layout,
        std::vector<p_record>&& records,
        bioseq::parallelism::memory_permit& adjacent_permit) {
        const std::uint64_t payload_capacity_bytes =
            records.capacity() > (std::numeric_limits<std::uint64_t>::max)() / sizeof(p_record)
                ? (std::numeric_limits<std::uint64_t>::max)()
                : static_cast<std::uint64_t>(records.capacity()) * sizeof(p_record);
        if (payload_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            adjacent_permit.bytes() != payload_capacity_bytes) {
            throw std::invalid_argument{
                "minimizer physical payload disagrees with its adjacent admission"};
        }
        physical_batch_owner_detail::transaction_scope transaction{
            adjacent_permit, payload_capacity_bytes};
        auto owner = std::allocate_shared<const std::vector<p_record>>(
            physical_batch_owner_detail::admitted_shared_owner_allocator<p_record>{},
            std::move(records));
        minimizer_physical_batch_owner result = from_shared_host<p_record>(
            layout, std::move(owner), transaction.owner_allocation_bytes());
        if (result.capacity_bytes() != adjacent_permit.bytes()) {
            throw std::logic_error{
                "minimizer physical owner disagrees with reconciled admission"};
        }
        transaction.commit();
        return result;
    }

    /**
     * Recover the selected concrete vector through a shared aliasing owner.
     * Every alias must remain nested inside the adjacent permit retained by
     * the enclosing prepared-query or aggregate-query owner.
     */
    template <generated_minimizer_physical_record p_record>
    [[nodiscard]] std::shared_ptr<const std::vector<p_record>> shared_host() const {
        if (!valid() || !layout_matches_record_<p_record>(layout_)) {
            throw std::invalid_argument{
                "minimizer physical owner was opened with the wrong generated record"};
        }
        return std::shared_ptr<const std::vector<p_record>>{
            vector_owner_, static_cast<const std::vector<p_record>*>(vector_owner_.get())};
    }

    /** Borrow a concrete-record dispatcher while this owner remains live. */
    [[nodiscard]] minimizer_physical_batch_view view() const noexcept {
        return valid() ? minimizer_physical_batch_view{
                             data_identity_,
                             size_,
                             layout_.record_id,
                             record_bytes_,
                             layout_.record_position_log2}
                       : minimizer_physical_batch_view{};
    }

    /** Return the exact generated layout selected before allocation. */
    [[nodiscard]] const minimizer_physical_layout_selection& layout() const noexcept {
        return layout_;
    }

    /** Return the immutable record count. */
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    /** Return the immutable concrete vector capacity. */
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    /** Return payload plus allocator-reported shared-owner capacity bytes. */
    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept {
        return capacity_bytes_;
    }

    /** Return retained bytes owned by the concrete vector payload allocation. */
    [[nodiscard]] std::uint64_t payload_capacity_bytes() const noexcept {
        return payload_capacity_bytes_;
    }

    /** Return the allocator-reported shared control/vector-object extent. */
    [[nodiscard]] std::uint64_t owner_allocation_bytes() const noexcept {
        return owner_allocation_bytes_;
    }

    /** Return whether this object carries a non-null concrete shared owner. */
    [[nodiscard]] bool valid() const noexcept {
        const auto* const descriptor = find_minimizer_physical_record(layout_.record_id);
        return vector_owner_ != nullptr && descriptor != nullptr &&
               descriptor->record_bytes == record_bytes_ && size_ <= capacity_ &&
               owner_allocation_bytes_ != 0u &&
               capacity_bytes_ >= payload_capacity_bytes_ &&
               capacity_bytes_ - payload_capacity_bytes_ == owner_allocation_bytes_ &&
               layout_.record_id != minimizer_physical_record_id::invalid;
    }

    /** Release the concrete vector owner and its decoded geometry. */
    void reset() noexcept {
        vector_owner_.reset();
        size_ = 0u;
        capacity_ = 0u;
        payload_capacity_bytes_ = 0u;
        owner_allocation_bytes_ = 0u;
        capacity_bytes_ = 0u;
        record_bytes_ = 0u;
        data_identity_ = nullptr;
        layout_ = {};
    }
};

/** Resolve a registered layout by its generated alphabet wire id. */
[[nodiscard]] constexpr minimizer_physical_layout_selection select_minimizer_physical_layout(
    const ::bioseq::simd::minimizer_algorithm algorithm,
    const std::uint64_t maximum_sequence_bp,
    const std::uint16_t runtime_kmer_size,
    const std::uint16_t runtime_window_size,
    const std::uint16_t alphabet_id,
    const std::uint8_t packed_storage_bits) {
    if (!minimizer_profile_has_alphabet_id(alphabet_id)) {
        throw std::invalid_argument(
            "alphabet has no registered minimizer profile wire id");
    }
    const auto required_position_log2 = minimizer_position_requirement(maximum_sequence_bp);
    const auto* profile = find_minimizer_runtime_profile(
        algorithm, alphabet_id, runtime_kmer_size, runtime_window_size,
        required_position_log2, packed_storage_bits);
    if (profile == nullptr) {
        throw std::invalid_argument("minimizer algorithm/profile has no registered physical layout");
    }
    const auto* record = find_minimizer_physical_record(profile->record_id);
    if (record == nullptr || record->position_log2 < required_position_log2) {
        throw std::invalid_argument("minimizer physical record binding does not fit its positions");
    }
    using algorithm_type = ::bioseq::simd::minimizer_algorithm;
    const auto family = algorithm == algorithm_type::bounded_nthash_simd ||
                                algorithm == algorithm_type::two_stack_nthash
        ? minimizer_physical_layout_family::fixed_u32_hash_legacy_k15
        : profile->effective_algorithm == algorithm_type::scalar
            ? minimizer_physical_layout_family::minimap2_scalar_reference_exact_k
            : minimizer_physical_layout_family::minimap2_reference_exact_k;
    return {
        .family = family,
        .record_id = profile->record_id,
        .requested_algorithm = profile->requested_algorithm,
        .effective_algorithm = profile->effective_algorithm,
        .alphabet_id = alphabet_id,
        .record_position_log2 = record->position_log2,
        .physical_kmer_size = record->physical_kmer_size,
        .hash_width = profile->hash_width,
        .packed_storage_bits = packed_storage_bits,
        .runtime_kmer_size = runtime_kmer_size,
        .runtime_window_size = runtime_window_size,
    };
}

/** Resolve a registered layout without allocation or a second shape registry. */
template <sequence_alphabet p_alphabet, class p_storage_unit>
    requires (std::same_as<p_storage_unit, std::uint32_t> ||
              std::same_as<p_storage_unit, std::uint64_t>)
[[nodiscard]] constexpr minimizer_physical_layout_selection select_minimizer_physical_layout(
    const ::bioseq::simd::minimizer_algorithm algorithm,
    const std::uint64_t maximum_sequence_bp,
    const std::uint16_t runtime_kmer_size,
    const std::uint16_t runtime_window_size) {
    if constexpr (!minimizer_profile_has_alphabet<p_alphabet>()) {
        throw std::invalid_argument(
            "alphabet has no registered minimizer profile wire id");
    } else {
        constexpr auto packed_storage_bits =
            static_cast<std::uint8_t>(sizeof(p_storage_unit) * 8u);
        return select_minimizer_physical_layout(
            algorithm, maximum_sequence_bp, runtime_kmer_size,
            runtime_window_size, minimizer_profile_alphabet_id<p_alphabet>(),
            packed_storage_bits);
    }
}

/**
 * @brief Selects the smallest packed record that preserves a minimizer shape.
 * @tparam p_max_sequence_log2 Position bits required by the sequence bound.
 * @tparam p_kmer_length K-mer length used to size the hash field.
 */
template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length>
struct minimizer_layout {
    /// Record-format parameter, independent of the biological sketch k.
    static constexpr std::size_t k_physical_kmer_size = p_kmer_length;
    static constexpr unsigned k_strand_bits = 1u;
    static constexpr unsigned k_position_bits = p_max_sequence_log2;
    static constexpr unsigned k_minimum_hash_bits =
        (2u * p_kmer_length < 56u) ? static_cast<unsigned>(2u * p_kmer_length) : 56u;
    static constexpr int k_uint16_hash_bits = 16 - 1 - static_cast<int>(k_position_bits);
    static constexpr int k_uint32_hash_bits = 32 - 1 - static_cast<int>(k_position_bits);
    static constexpr int k_uint64_hash_bits = 64 - 1 - static_cast<int>(k_position_bits);
    static constexpr int k_uint128_hash_bits =
        128 - 1 - static_cast<int>(k_position_bits) - 8;
    static constexpr unsigned k_storage_bits = static_cast<unsigned>(
        minimizer_record_storage_bits(p_max_sequence_log2, p_kmer_length));
    static constexpr unsigned k_hash_bits =
        (k_storage_bits == 16u) ? static_cast<unsigned>(k_uint16_hash_bits)
        : (k_storage_bits == 32u) ? static_cast<unsigned>(k_uint32_hash_bits)
        : (k_storage_bits == 64u) ? static_cast<unsigned>(k_uint64_hash_bits)
        : static_cast<unsigned>(k_uint128_hash_bits);
    static constexpr unsigned k_span_bits = k_storage_bits == 128u ? 8u : 0u;
    using storage_type =
        std::conditional_t<k_storage_bits == 16u, std::uint16_t,
        std::conditional_t<k_storage_bits == 32u, std::uint32_t,
        std::conditional_t<k_storage_bits == 64u, std::uint64_t, void>>>;
};

/**
 * @brief Stores one exact minimizer shape in its selected packed layout.
 * @tparam p_max_sequence_log2 Position bits carried by the record.
 * @tparam p_kmer_length K-mer length used to size the hash field.
 * @tparam p_is_128_bit Selects the two-word layout when `true`.
 */
template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length,
         bool p_is_128_bit =
             (minimizer_layout<p_max_sequence_log2, p_kmer_length>::k_storage_bits == 128u)>
struct minimizer_representation;

/**
 * @brief Stores hash, position, and strand in one unsigned integer.
 * @tparam p_max_sequence_log2 Position bits carried by the record.
 * @tparam p_kmer_length K-mer length used to size the hash field.
 */
template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length>
struct minimizer_representation<p_max_sequence_log2, p_kmer_length, false> {
    using layout_type = minimizer_layout<p_max_sequence_log2, p_kmer_length>;
    using storage_type = typename layout_type::storage_type;
    static_assert(!std::is_void_v<storage_type>);

    static constexpr unsigned k_strand_shift = 0u;
    static constexpr unsigned k_position_shift = layout_type::k_strand_bits;
    static constexpr unsigned k_hash_shift =
        layout_type::k_strand_bits + layout_type::k_position_bits;
    static constexpr storage_type k_hash_field_mask =
        (layout_type::k_hash_bits >= sizeof(storage_type) * 8u)
            ? static_cast<storage_type>(~storage_type{0})
            : static_cast<storage_type>(
                (storage_type{1} << layout_type::k_hash_bits) - storage_type{1});
    static constexpr storage_type k_position_field_mask =
        (layout_type::k_position_bits >= sizeof(storage_type) * 8u)
            ? static_cast<storage_type>(~storage_type{0})
            : static_cast<storage_type>(
                (storage_type{1} << layout_type::k_position_bits) - storage_type{1});
    static constexpr storage_type k_strand_field_mask = storage_type{1};
    static constexpr storage_type k_hash_mask =
        static_cast<storage_type>(k_hash_field_mask << k_hash_shift);
    static constexpr storage_type k_position_strand_mask =
        static_cast<storage_type>(~k_hash_mask);

    storage_type value_;

    /** Construct the zero-valued wire record. */
    constexpr minimizer_representation() noexcept : value_(0) {}

    /** Pack hash, last position, and strand into the selected storage word. */
    constexpr minimizer_representation(
        const std::uint64_t kmer_hash,
        const std::uint64_t last_position,
        const std::uint64_t strand_bit) noexcept
        : value_(static_cast<storage_type>(
            ((static_cast<storage_type>(kmer_hash) & k_hash_field_mask)
                << k_hash_shift)
            | ((static_cast<storage_type>(last_position)
                    & k_position_field_mask)
                << k_position_shift)
            | (static_cast<storage_type>(strand_bit) & k_strand_field_mask)))
    {}

    /** Restore a record from its exact packed storage word. */
    [[nodiscard]] static constexpr minimizer_representation from_raw_storage(
        const storage_type value) noexcept
    {
        minimizer_representation result;
        result.value_ = value;
        return result;
    }
    /** Canonical k-mer hash. */
    [[nodiscard]] constexpr std::uint64_t kmer() const noexcept {
        return static_cast<std::uint64_t>(value_) >> k_hash_shift;
    }
    /** Zero-based position of the k-mer's final symbol. */
    [[nodiscard]] constexpr std::uint64_t last_pos() const noexcept {
        return (static_cast<std::uint64_t>(value_) >> k_position_shift)
            & static_cast<std::uint64_t>(k_position_field_mask);
    }
    /** Canonical strand bit. */
    [[nodiscard]] constexpr std::uint64_t strand() const noexcept {
        return static_cast<std::uint64_t>(value_) & 1ULL;
    }
    /** Packed position and strand bits. */
    [[nodiscard]] constexpr std::uint64_t pos_strand_encoded() const noexcept {
        return static_cast<std::uint64_t>(value_)
            & static_cast<std::uint64_t>(k_position_strand_mask);
    }
    /** Exact packed storage word. */
    [[nodiscard]] constexpr storage_type raw_value() const noexcept {
        return value_;
    }
    /** Sentinel that compares above every valid hash. */
    [[nodiscard]] static constexpr minimizer_representation maximum() noexcept {
        return from_raw_storage(k_hash_mask);
    }
    /** Whether this record carries the maximum sentinel hash. */
    [[nodiscard]] constexpr bool is_max() const noexcept {
        return (value_ & k_hash_mask) == k_hash_mask;
    }
    /** Replace this record with the maximum sentinel. */
    constexpr void set_max() noexcept {
        value_ = k_hash_mask;
    }
    /** @brief Orders records by their masked canonical hash. */
    [[nodiscard]] constexpr auto operator<=> (
        const minimizer_representation& other) const noexcept
    {
        return (value_ & k_hash_mask) <=> (other.value_ & k_hash_mask);
    }
    /** @brief Tests canonical-hash equality without position identity. */
    [[nodiscard]] constexpr bool operator==(
        const minimizer_representation& other) const noexcept
    {
        return (value_ & k_hash_mask) == (other.value_ & k_hash_mask);
    }
    /** Whether equal hashes carry different position/strand identity. */
    [[nodiscard]] static constexpr bool does_sketch_match(
        const minimizer_representation& first,
        const minimizer_representation& second) noexcept
    {
        return ((first.value_ ^ second.value_) & k_hash_mask) == 0u
            && first.value_ != second.value_;
    }
};

/**
 * @brief Stores hash, span, position, and strand in two 64-bit words.
 * @tparam p_max_sequence_log2 Position bits carried by the record.
 * @tparam p_kmer_length K-mer length used as the default span.
 */
template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length>
struct minimizer_representation<p_max_sequence_log2, p_kmer_length, true> {
    using layout_type = minimizer_layout<p_max_sequence_log2, p_kmer_length>;
    static_assert(layout_type::k_position_bits + 1u <= 64u);

    static constexpr unsigned k_high_span_shift = 0u;
    static constexpr unsigned k_high_hash_shift = 8u;
    static constexpr std::uint64_t k_hash_field_mask =
        layout_type::k_hash_bits >= 64u
            ? ~std::uint64_t{0}
            : (std::uint64_t{1} << layout_type::k_hash_bits) - std::uint64_t{1};
    static constexpr std::uint64_t k_position_field_mask =
        layout_type::k_position_bits >= 64u
            ? ~std::uint64_t{0}
            : (std::uint64_t{1} << layout_type::k_position_bits) - std::uint64_t{1};
    static constexpr std::uint64_t k_span_field_mask = std::uint64_t{0xFFu};
    static constexpr std::uint64_t k_high_hash_mask =
        k_hash_field_mask << k_high_hash_shift;
    static constexpr std::uint64_t k_low_position_mask = k_position_field_mask << 1u;

    std::uint64_t low_;
    std::uint64_t high_;

    /** Construct the zero-valued two-word record. */
    constexpr minimizer_representation() noexcept : low_(0), high_(0) {}

    /** Pack hash, position, strand, and span into two uint64 words. */
    constexpr minimizer_representation(
        const std::uint64_t kmer_hash,
        const std::uint64_t last_position,
        const std::uint64_t strand_bit,
        const std::uint64_t span = static_cast<std::uint64_t>(p_kmer_length)) noexcept
        : low_(((last_position & k_position_field_mask) << 1u)
                | (strand_bit & 1ULL)),
          high_(((kmer_hash & k_hash_field_mask) << k_high_hash_shift)
                | (span & k_span_field_mask))
    {}

    /** Low packed position/strand word. */
    [[nodiscard]] constexpr std::uint64_t data_low_64() const noexcept {
        return low_;
    }
    /** High packed hash/span word. */
    [[nodiscard]] constexpr std::uint64_t data_high_64() const noexcept {
        return high_;
    }
    /** @brief Returns the canonical hash field. */
    [[nodiscard]] constexpr std::uint64_t kmer() const noexcept {
        return high_ >> k_high_hash_shift;
    }
    /** @brief Returns the stored k-mer span. */
    [[nodiscard]] constexpr std::uint64_t span() const noexcept {
        return high_ & k_span_field_mask;
    }
    /** @brief Returns the zero-based position of the final k-mer symbol. */
    [[nodiscard]] constexpr std::uint64_t last_pos() const noexcept {
        return (low_ >> 1u) & k_position_field_mask;
    }
    /** @brief Returns the canonical strand bit. */
    [[nodiscard]] constexpr std::uint64_t strand() const noexcept {
        return low_ & 1ULL;
    }
    /** @brief Returns the packed position and strand word. */
    [[nodiscard]] constexpr std::uint64_t pos_strand_encoded() const noexcept {
        return low_;
    }
    /** @brief Returns the sentinel that compares above every valid hash. */
    [[nodiscard]] static constexpr minimizer_representation maximum() noexcept {
        minimizer_representation result;
        result.high_ = k_high_hash_mask;
        return result;
    }
    /** @brief Reports whether the record carries the maximum sentinel hash. */
    [[nodiscard]] constexpr bool is_max() const noexcept {
        return (high_ & k_high_hash_mask) == k_high_hash_mask;
    }
    /** @brief Replaces the record with the maximum sentinel. */
    constexpr void set_max() noexcept {
        high_ = k_high_hash_mask;
        low_ = 0u;
    }
    /** @brief Orders records by their masked canonical hash. */
    [[nodiscard]] constexpr auto operator<=> (
        const minimizer_representation& other) const noexcept
    {
        return (high_ & k_high_hash_mask) <=> (other.high_ & k_high_hash_mask);
    }
    /** @brief Tests canonical-hash equality without span or position identity. */
    [[nodiscard]] constexpr bool operator==(
        const minimizer_representation& other) const noexcept
    {
        return (high_ & k_high_hash_mask) == (other.high_ & k_high_hash_mask);
    }
    /** @brief Tests equal hashes with distinct span or position identity. */
    [[nodiscard]] static constexpr bool does_sketch_match(
        const minimizer_representation& first,
        const minimizer_representation& second) noexcept
    {
        constexpr std::uint64_t k_high_identity_mask = ~k_span_field_mask;
        return ((first.high_ ^ second.high_) & k_high_hash_mask) == 0u
            && ((first.high_ & k_high_identity_mask)
                    != (second.high_ & k_high_identity_mask)
                || first.low_ != second.low_);
    }
};

inline minimizer_physical_record_value
minimizer_physical_batch_view::at(const std::size_t ordinal) const {
    if (ordinal >= size_) {
        throw std::out_of_range{"minimizer physical batch ordinal is invalid"};
    }
    return at_unchecked(ordinal);
}

/** Canonical minimap2-compatible 64-bit sketch record. */
using minimap_sketch_data = minimizer_representation<28u, 15u>;

static_assert(std::same_as<minimap_sketch_data, minimizer_representation<28u, 15u>>);
static_assert(sizeof(minimap_sketch_data) == sizeof(std::uint64_t));
static_assert(alignof(minimap_sketch_data) == alignof(std::uint64_t));
static_assert(std::is_standard_layout_v<minimap_sketch_data>);
static_assert(std::is_trivially_copyable_v<minimap_sketch_data>);

using std::size_t;
using std::uint64_t;

/**
 * @brief Maximum sequence length representable without masking a position bit.
 * @tparam p_minimizer_type Packed minimizer record defining the position mask.
 */
template <class p_minimizer_type>
inline constexpr bioseq::storage::sequence_size_t
k_max_minimizer_sequence_length = []() constexpr {
    constexpr auto mask = static_cast<bioseq::storage::sequence_size_t>(
        p_minimizer_type::k_position_field_mask);
    if constexpr (mask
            == (std::numeric_limits<
                bioseq::storage::sequence_size_t>::max)()) {
        return mask;
    } else {
        return static_cast<bioseq::storage::sequence_size_t>(mask + 1u);
    }
}();

/**
 * @brief Tests whether every zero-based query position fits the record.
 * @tparam p_minimizer_type Packed minimizer record defining the position mask.
 * @param query_length Query length in biological symbols.
 * @return `true` when the final position cannot be truncated.
 */
template <class p_minimizer_type>
[[nodiscard]] inline constexpr bool is_minimizer_query_length_representable(
        bioseq::storage::sequence_size_t query_length) noexcept {
    return query_length <= k_max_minimizer_sequence_length<p_minimizer_type>;
}

/**
 * @brief Rejects a query whose final position exceeds the record field.
 * @tparam p_minimizer_type Packed minimizer record defining the position mask.
 * @param query_length Query length in biological symbols.
 * @throws std::length_error If the final position would be truncated.
 */
template <class p_minimizer_type>
inline void require_representable_minimizer_query_length(
        bioseq::storage::sequence_size_t query_length) {
    if (!is_minimizer_query_length_representable<p_minimizer_type>(query_length)) {
        throw std::length_error(
            "query length exceeds the selected minimizer position field");
    }
}

static_assert(
    k_max_minimizer_sequence_length<minimap_sketch_data>
        == (bioseq::storage::sequence_size_t{1u} << 28u),
    "minimap_sketch_data must accept 2^28 bases and reject 2^28 + 1");
static_assert(is_minimizer_query_length_representable<minimap_sketch_data>(
    bioseq::storage::sequence_size_t{1u} << 28u));
static_assert(!is_minimizer_query_length_representable<minimap_sketch_data>(
    (bioseq::storage::sequence_size_t{1u} << 28u) + 1u));

static_assert(sizeof(minimap_sketch_data) == 8,
              "Canonical minimap_sketch_data must remain a single uint64_t");
static_assert(alignof(minimap_sketch_data) == 8);
static_assert(std::is_standard_layout_v<minimap_sketch_data>);
static_assert(std::is_trivially_copyable_v<minimap_sketch_data>);
static_assert(std::is_trivially_destructible_v<minimap_sketch_data>);

// Storage and wire consumers require the canonical 35:28:1 bit layout.
static_assert(minimizer_layout<28u, 15u>::k_storage_bits == 64u,
    "minimap_sketch_data = minimizer_representation<28, 15> must remain a 64-bit storage type. "
    "Postgres sidecar / dispatch_buffer rely on sizeof(minimap_sketch_data) == 8 "
    "and the bit-layout (hash:35 | pos:28 | strand:1) is hardcoded in the wire "
    "protocol.  Update the sidecar wire format before relaxing this constraint.");

// Pin the canonical layout independently of other representation shapes.
static_assert(minimap_sketch_data::layout_type::k_position_bits == 28u);
static_assert(minimap_sketch_data::layout_type::k_hash_bits == 35u);
static_assert(minimap_sketch_data::layout_type::k_strand_bits == 1u);
static_assert(minimap_sketch_data::k_hash_shift == 29u);
static_assert(minimap_sketch_data::k_hash_mask == 0xFFFFFFFFE0000000ULL);

} // namespace bioseq::minimizers
