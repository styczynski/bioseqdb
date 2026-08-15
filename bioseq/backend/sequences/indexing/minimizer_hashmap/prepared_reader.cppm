/**
 * @file minimizer_hashmap/prepared_reader.cppm
 * @brief Validated grouped-SoA admission and prepared query readers.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:prepared_reader;

import std;
import :exact_simd;
import :grouped_soa;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export namespace bioseq::indexing {

/// Decoded grouped-SoA control with its half-open position interval.
struct grouped_soa_decoded_control {
    std::uint64_t hash = 0;
    std::uint64_t position_begin = 0;
    std::uint64_t position_end = 0;
    std::uint64_t saturated_count = 0;

    [[nodiscard]] bool operator==(const grouped_soa_decoded_control&) const = default;
};

/// Callback-scoped qualified row emitted directly to PostgreSQL/fake storage.
struct minimizer_generation_row_view {
    index_chunk_address address{};
    std::span<const std::byte> bytes;
    std::uint64_t row_checksum = 0;
};

/// Exact failure site retained alongside the stable wire error
/// `minimizer_generation_encoding_error::admission`.  The numeric wire-error ABI does
/// not change. These stats explain which owner or allocation was refused.
enum class grouped_soa_query_admission_stage : std::uint8_t {
    none,
    identity_and_directory_reservation,
    query_hash_reservation,
    query_hash_allocation,
    query_hash_reconcile,
    query_route_reservation,
    query_route_allocation,
    query_route_reconcile,
    control_payload_reservation,
    control_payload_allocation,
    control_payload_reconcile,
    control_decode_reservation,
    control_decode_allocation,
    control_decode_reconcile,
    position_image_reservation,
    position_image_allocation,
    position_image_reconcile,
};

/// Return the stable diagnostic name for an admission substage.
[[nodiscard]] constexpr std::string_view
grouped_soa_query_admission_substage_name(grouped_soa_query_admission_stage substage) noexcept {
    switch (substage) {
    case grouped_soa_query_admission_stage::none:
        return "none";
    case grouped_soa_query_admission_stage::identity_and_directory_reservation:
        return "identity_and_directory_reservation";
    case grouped_soa_query_admission_stage::query_hash_reservation:
        return "query_hash_reservation";
    case grouped_soa_query_admission_stage::query_hash_allocation:
        return "query_hash_allocation";
    case grouped_soa_query_admission_stage::query_hash_reconcile:
        return "query_hash_reconcile";
    case grouped_soa_query_admission_stage::query_route_reservation:
        return "query_route_reservation";
    case grouped_soa_query_admission_stage::query_route_allocation:
        return "query_route_allocation";
    case grouped_soa_query_admission_stage::query_route_reconcile:
        return "query_route_reconcile";
    case grouped_soa_query_admission_stage::control_payload_reservation:
        return "control_payload_reservation";
    case grouped_soa_query_admission_stage::control_payload_allocation:
        return "control_payload_allocation";
    case grouped_soa_query_admission_stage::control_payload_reconcile:
        return "control_payload_reconcile";
    case grouped_soa_query_admission_stage::control_decode_reservation:
        return "control_decode_reservation";
    case grouped_soa_query_admission_stage::control_decode_allocation:
        return "control_decode_allocation";
    case grouped_soa_query_admission_stage::control_decode_reconcile:
        return "control_decode_reconcile";
    case grouped_soa_query_admission_stage::position_image_reservation:
        return "position_image_reservation";
    case grouped_soa_query_admission_stage::position_image_allocation:
        return "position_image_allocation";
    case grouped_soa_query_admission_stage::position_image_reconcile:
        return "position_image_reconcile";
    }
    return "unknown";
}

/// Frozen stats copied out before a failed reader releases its admitted
/// storage. Capacity fields describe actual live owners at the failure site.
/// the controller fields come from the exact rejection snapshot when one
/// exists, otherwise from a read-only post-failure diagnostic snapshot.
struct grouped_soa_query_admission_stats {
    grouped_soa_query_admission_stage substage = grouped_soa_query_admission_stage::none;
    bioseq::parallelism::memory_admission_failure_reason controller_reason =
        bioseq::parallelism::memory_admission_failure_reason::none;
    std::uint64_t retained_position_count = 0;
    std::uint64_t retained_position_bytes = 0;
    std::uint64_t allocation_headroom_bytes = 0;
    std::uint64_t projected_bytes = 0;
    std::uint64_t requested_growth_bytes = 0;
    std::uint64_t owned_capacity_bytes = 0;
    std::uint64_t permit_bytes = 0;
    std::uint64_t controller_reserved_bytes = 0;
    std::uint64_t controller_budget_ceiling_bytes = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t controller_allocatable_after_reservations_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t process_footprint_bytes = 0;
    std::uint64_t system_available_bytes = 0;
    std::uint64_t system_total_bytes = 0;
    std::uint64_t process_soft_cap_bytes = 0;
    std::uint64_t query_record_count = 0;
    std::uint64_t query_record_capacity = 0;
    std::uint64_t query_hash_count = 0;
    std::uint64_t query_hash_capacity = 0;
    std::uint64_t selected_extent_count = 0;
    std::uint64_t selected_extent_capacity = 0;
    std::uint64_t selected_control_capacity_bytes = 0;
    std::uint64_t control_payload_capacity_bytes = 0;
    std::uint64_t releasable_protocol_query_hash_bytes = 0;
    bool controller_failure_snapshot_present = false;
    bool injected_failure = false;
};

/// Deterministic portable allocation-failure hook.  Production callers leave
/// this at `none`. Tests select one exact substage without attempting a huge
/// allocation or depending on an operating-system overcommit policy.
struct grouped_soa_query_reader_options {
    grouped_soa_query_admission_stage fail_admission_substage =
        grouped_soa_query_admission_stage::none;
};

/// Exact reservation and ownership plan for the retained position image.
struct grouped_soa_position_image_plan {
    std::uint64_t retained_position_count = 0;
    std::uint64_t retained_position_bytes = 0;
    std::uint64_t allocation_headroom_bytes = 0;
    std::uint64_t owned_capacity_bytes = 0;
    std::uint64_t projected_bytes = 0;
};

/// Cached byte totals for the trait-driven prepared reader. These fields
/// account for the active `memory_permit`. Every field names one live
/// allocation owner, and `total_bytes` is updated in constant time whenever
/// that owner changes.
struct protocol_owned_bytes_ledger {
    std::uint64_t directory_bytes = 0;
    std::uint64_t query_hash_bytes = 0;
    std::uint64_t query_record_bytes = 0;
    std::uint64_t radix_offset_bytes = 0;
    std::uint64_t global_position_bytes = 0;
    std::uint64_t ordinal_map_bytes = 0;
    std::uint64_t selected_extent_vector_bytes = 0;
    std::uint64_t selected_extent_owner_bytes = 0;
    std::uint64_t query_record_position_bytes = 0;
    std::uint64_t control_scratch_bytes = 0;
    std::uint64_t active_session_bytes = 0;
    std::uint64_t total_bytes = 0;

    protocol_owned_bytes_ledger() = default;
    protocol_owned_bytes_ledger(const protocol_owned_bytes_ledger&) = default;
    protocol_owned_bytes_ledger& operator=(const protocol_owned_bytes_ledger&) = default;

    protocol_owned_bytes_ledger(protocol_owned_bytes_ledger&& other) noexcept
        : protocol_owned_bytes_ledger(static_cast<const protocol_owned_bytes_ledger&>(other)) {
        other.reset();
    }

    protocol_owned_bytes_ledger& operator=(protocol_owned_bytes_ledger&& other) noexcept {
        if (this != &other) {
            *this = static_cast<const protocol_owned_bytes_ledger&>(other);
            other.reset();
        }
        return *this;
    }

    /// Replace one already-accounted owner. The caller publishes its named
    /// field only after this checked total update succeeds.
    [[nodiscard]] bool replace_checked(std::uint64_t old_bytes, std::uint64_t new_bytes) noexcept {
        if (old_bytes > total_bytes)
            return false;
        const std::uint64_t without_old = total_bytes - old_bytes;
        if (new_bytes > (std::numeric_limits<std::uint64_t>::max)() - without_old) {
            return false;
        }
        total_bytes = without_old + new_bytes;
        return true;
    }

    /// Check a temporary coexistence peak before asking the active permit
    /// to grow. No ownership is published by this helper.
    [[nodiscard]] bool add_temporary_peak_checked(std::uint64_t peak_bytes) const noexcept {
        return peak_bytes <= (std::numeric_limits<std::uint64_t>::max)() - total_bytes;
    }

    /// Check that named owners sum exactly to the cached total.
    [[nodiscard]] bool field_sum_holds() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t sum = directory_bytes;
        sum = saturating_add(sum, query_hash_bytes);
        sum = saturating_add(sum, query_record_bytes);
        sum = saturating_add(sum, radix_offset_bytes);
        sum = saturating_add(sum, global_position_bytes);
        sum = saturating_add(sum, ordinal_map_bytes);
        sum = saturating_add(sum, selected_extent_vector_bytes);
        sum = saturating_add(sum, selected_extent_owner_bytes);
        sum = saturating_add(sum, query_record_position_bytes);
        sum = saturating_add(sum, control_scratch_bytes);
        sum = saturating_add(sum, active_session_bytes);
        return sum != (std::numeric_limits<std::uint64_t>::max)() && sum == total_bytes;
    }

    /// Clear every named owner and the cached total.
    void reset() noexcept {
        directory_bytes = 0;
        query_hash_bytes = 0;
        query_record_bytes = 0;
        radix_offset_bytes = 0;
        global_position_bytes = 0;
        ordinal_map_bytes = 0;
        selected_extent_vector_bytes = 0;
        selected_extent_owner_bytes = 0;
        query_record_position_bytes = 0;
        control_scratch_bytes = 0;
        active_session_bytes = 0;
        total_bytes = 0;
    }
};

/// Pure checked byte plan used before any position-image reservation or
/// allocation.  Tests can exercise adversarial counts without relying on
/// virtual-memory overcommit or constructing a correspondingly huge payload.
[[nodiscard]] inline std::expected<grouped_soa_position_image_plan,
                                   minimizer_generation_encoding_error>
plan_grouped_soa_position_image(std::uint64_t retained_position_count,
                                std::uint64_t owned_capacity_bytes) noexcept {
    const std::uint64_t retained_position_bytes =
        bioseq::common::saturating_mul(retained_position_count, sizeof(std::uint64_t));
    // The grouped-SoA image is one exact owned array: unlike a vector reserve,
    // its requested element count and admitted ownership are both exactly P.
    // Keep headroom explicit at zero so SQL stats can distinguish this
    // path from conservative vector preallocation.
    constexpr std::uint64_t allocation_headroom_bytes = 0u;
    const std::uint64_t projected_bytes =
        bioseq::common::saturating_add(owned_capacity_bytes, retained_position_bytes);
    if (retained_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        projected_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return grouped_soa_position_image_plan{
        .retained_position_count = retained_position_count,
        .retained_position_bytes = retained_position_bytes,
        .allocation_headroom_bytes = allocation_headroom_bytes,
        .owned_capacity_bytes = owned_capacity_bytes,
        .projected_bytes = projected_bytes,
    };
}

namespace minimizer_prepared_reader_detail {
/// Trait-directed prepared-query implementation shared by public readers.
template <class p_traits> class trait_driven_prepared_query_core;
} // namespace minimizer_prepared_reader_detail

/// Unforgeable stats that the address-bound row checksum was checked before
/// callback-scoped bytes reached a prepared reader. Ordinary importers can
/// obtain one only through validate_minimizer_generation_row_receipt(), which
/// performs that check. PostgreSQL retains that same receipt beside the loaded
/// row, so consumers reuse the single validation pass without special access.
class validated_minimizer_generation_row_receipt {
  private:
    index_chunk_address address_{};
    std::span<const std::byte> bytes_{};
    std::uint64_t row_checksum_ = 0;
    std::uint64_t authority_seal_ = 0;
    bool live_ = false;

    [[nodiscard]] static std::uint64_t
    authority_seal(const validated_minimizer_generation_row_receipt* self,
                   const index_chunk_address& address,
                   std::span<const std::byte> bytes,
                   std::uint64_t row_checksum) noexcept {
        std::uint64_t value = 0x9E3779B97F4A7C15ULL;
        const auto mix = [&value](std::uint64_t word) noexcept {
            value ^= word + 0x9E3779B97F4A7C15ULL + (value << 6u) + (value >> 2u);
        };
        mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(self)));
        mix(address.generation_id);
        mix(static_cast<std::uint32_t>(address.position_segment));
        mix(static_cast<std::uint64_t>(address.sub_key));
        mix(address.chunk_no);
        mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes.data())));
        mix(bytes.size());
        mix(row_checksum);
        return value;
    }

    validated_minimizer_generation_row_receipt(index_chunk_address address,
                                               std::span<const std::byte> bytes,
                                               std::uint64_t row_checksum) noexcept
        : address_(address), bytes_(bytes), row_checksum_(row_checksum),
          authority_seal_(authority_seal(this, address, bytes, row_checksum)), live_(true) {}

    void invalidate() noexcept {
        address_ = {};
        bytes_ = {};
        row_checksum_ = 0;
        authority_seal_ = 0;
        live_ = false;
    }

    friend std::expected<validated_minimizer_generation_row_receipt,
                         minimizer_generation_encoding_error>
    validate_minimizer_generation_row_receipt(const minimizer_generation_row_view&) noexcept;
    template <class p_traits>
    friend class minimizer_prepared_reader_detail::trait_driven_prepared_query_core;
    template <class p_visitor>
    friend bool visit_validated_minimizer_generation_row(
        const validated_minimizer_generation_row_receipt&,
        p_visitor&&) noexcept(std::is_nothrow_invocable_v<p_visitor&,
                                                          const index_chunk_address&,
                                                          std::span<const std::byte>,
                                                          std::uint64_t>);

  public:
    validated_minimizer_generation_row_receipt() = delete;
    validated_minimizer_generation_row_receipt(const validated_minimizer_generation_row_receipt&) =
        delete;
    validated_minimizer_generation_row_receipt&
    operator=(const validated_minimizer_generation_row_receipt&) = delete;
    validated_minimizer_generation_row_receipt&
    operator=(validated_minimizer_generation_row_receipt&&) = delete;

    validated_minimizer_generation_row_receipt(
        validated_minimizer_generation_row_receipt&& other) noexcept
        : address_(other.address_), bytes_(other.bytes_), row_checksum_(other.row_checksum_),
          authority_seal_(authority_seal(this, address_, bytes_, row_checksum_)),
          live_(other.live_) {
        other.invalidate();
    }

    ~validated_minimizer_generation_row_receipt() {
        invalidate();
    }

    /// Return whether the move-bound authority seal still matches this receipt.
    [[nodiscard]] bool has_valid_authority() const noexcept {
        return live_ && authority_seal_ == authority_seal(this, address_, bytes_, row_checksum_);
    }

  private:
    [[nodiscard]] constexpr const index_chunk_address& address() const noexcept {
        return address_;
    }
    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr std::uint64_t row_checksum() const noexcept {
        return row_checksum_;
    }
};

/// Visit an authenticated row without exporting the receipt's representation.
/// The byte span remains callback-scoped. Callers cannot retain authority or
/// mutate the receipt, and an invalid/moved-from receipt is never visited.
template <class p_visitor>
[[nodiscard]] bool visit_validated_minimizer_generation_row(
    const validated_minimizer_generation_row_receipt& receipt,
    p_visitor&& visitor) noexcept(std::is_nothrow_invocable_v<p_visitor&,
                                                              const index_chunk_address&,
                                                              std::span<const std::byte>,
                                                              std::uint64_t>) {
    if (!receipt.has_valid_authority())
        return false;
    std::forward<p_visitor>(visitor)(receipt.address_, receipt.bytes_, receipt.row_checksum_);
    return true;
}

/// Authenticate one callback-scoped row before granting the trusted-reader
/// fast path. This is deliberately the only exported receipt construction
/// path, and it never returns authority for an unchecked or mismatched CRC.
[[nodiscard]] inline std::expected<validated_minimizer_generation_row_receipt,
                                   minimizer_generation_encoding_error>
validate_minimizer_generation_row_receipt(const minimizer_generation_row_view& row) noexcept {
    if (row.bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t expected_checksum =
        crc64_index_chunk_row(row.address.generation_id,
                              row.address.position_segment,
                              row.address.sub_key,
                              row.address.chunk_no,
                              static_cast<std::uint32_t>(row.bytes.size()),
                              row.bytes);
    if (row.row_checksum != expected_checksum) {
        return std::unexpected(minimizer_generation_encoding_error::checksum);
    }
    return validated_minimizer_generation_row_receipt{row.address, row.bytes, row.row_checksum};
}

static_assert(!std::is_default_constructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_copy_constructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_copy_assignable_v<validated_minimizer_generation_row_receipt>);
static_assert(std::is_move_constructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_move_assignable_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_copyable_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_move_constructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_destructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_aggregate_v<validated_minimizer_generation_row_receipt>);

namespace minimizer_prepared_reader_detail {

template <minimizer_generation_layout p_layout> struct codec_traits;

template <> struct codec_traits<minimizer_generation_layout::grouped_soa_segment_local> {
    static constexpr minimizer_generation_layout layout =
        minimizer_generation_layout::grouped_soa_segment_local;
    using control_type = grouped_soa_decoded_control;
    using position_type = std::uint64_t;
    using hash_type = std::uint64_t;
    static constexpr bool global_controls_then_positions = true;
};

/// Shared move-only admitted identity/directory owner used by both prepared
/// grouped readers. codec_traits is the storage-layout authority. Callers
/// cannot decode a directory under a different layout and relabel the facade.
template <class p_traits> class admitted_prepared_reader_core {
  protected:
    bioseq::parallelism::memory_permit permit_{};
    grouped_soa_directory_view directory_{};
    minimizer_generation_head head_{};
    minimizer_generation_manifest manifest_{};
    expected_minimizer_generation_identity expected_{};
    grouped_soa_minimizer_profile profile_{
        .layout = minimizer_generation_layout::invalid,
    };

    admitted_prepared_reader_core() = default;
    admitted_prepared_reader_core(admitted_prepared_reader_core&&) noexcept = default;
    admitted_prepared_reader_core& operator=(admitted_prepared_reader_core&&) noexcept = default;
    admitted_prepared_reader_core(const admitted_prepared_reader_core&) = delete;
    admitted_prepared_reader_core& operator=(const admitted_prepared_reader_core&) = delete;

    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    begin_admitted_core(
        admitted_prepared_reader_core& destination,
        bioseq::parallelism::memory_admission_controller& admission,
        const minimizer_generation_head& head,
        const minimizer_generation_manifest& manifest,
        const expected_minimizer_generation_identity& expected,
        const grouped_soa_minimizer_profile& profile,
        std::span<const std::byte> encoded_directory,
        std::uint64_t additional_logical_bytes = 0,
        bioseq::parallelism::memory_admission_failure* admission_failure = nullptr) {
        const bool destination_is_fresh =
            !destination.permit_ && destination.directory_.owned_capacity_bytes() == 0u &&
            destination.head_.layout == minimizer_generation_layout::invalid &&
            destination.manifest_.layout == minimizer_generation_layout::invalid &&
            destination.expected_.layout == minimizer_generation_layout::invalid &&
            destination.profile_.layout == minimizer_generation_layout::invalid;
        DEBUG_ASSERT(destination_is_fresh);
        if (!destination_is_fresh) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (head.layout != p_traits::layout || manifest.layout != p_traits::layout ||
            expected.layout != p_traits::layout || profile.layout != p_traits::layout ||
            !validate_grouped_soa_minimizer_profile(profile)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (auto validated = validate_minimizer_generation_storage_contract(manifest, expected);
            !validated) {
            return std::unexpected(validated.error());
        }
        const std::size_t stride = minimizer_directory_entry_encoded_bytes(profile);
        if (encoded_directory.size() % stride != 0) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        const std::uint64_t entry_count = encoded_directory.size() / stride;
        std::uint64_t logical_bytes = bioseq::common::saturating_add(
            encoded_directory.size(),
            bioseq::common::saturating_mul(entry_count,
                                           sizeof(grouped_soa_bucket_directory_entry)));
        logical_bytes = bioseq::common::saturating_add(logical_bytes, additional_logical_bytes);
        if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        auto permit = admission.try_reserve(logical_bytes, admission_failure);
        if (!permit) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        auto directory = grouped_soa_directory_view::decode_admitted(
            encoded_directory, profile, *permit, admission_failure);
        if (!directory)
            return std::unexpected(directory.error());
        if (auto valid = validate_minimizer_generation_metadata(head, manifest, *directory);
            !valid) {
            return std::unexpected(valid.error());
        }
        // Commit only after every fallible validation has completed. Until
        // this point the local directory is destroyed before the local permit
        // releases, and on success ownership moves exactly once into the
        // already-constructed reader base subobject.
        destination.permit_ = std::move(*permit);
        destination.directory_ = std::move(*directory);
        destination.head_ = head;
        destination.manifest_ = manifest;
        destination.expected_ = expected;
        destination.profile_ = profile;
        return {};
    }

    [[nodiscard]] std::uint64_t core_owned_capacity_bytes() const noexcept {
        return directory_.owned_capacity_bytes();
    }

    void release_core_noexcept() noexcept {
        directory_ = grouped_soa_directory_view{};
        permit_.release();
    }
};

} // namespace minimizer_prepared_reader_detail

/// Decode and validate one complete grouped-SoA control extent. Padding bytes are
/// part of the corruption contract and must remain zero.
[[nodiscard]] inline std::expected<std::vector<grouped_soa_decoded_control>,
                                   minimizer_generation_encoding_error>
decode_grouped_soa_controls(std::span<const std::byte> input,
                            const grouped_soa_bucket_directory_entry& entry,
                            const grouped_soa_minimizer_profile& profile) {
    if (profile.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        entry.control_encoded_bytes != input.size()) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                       profile.hash_width,
                                                       entry.shape_offset_width,
                                                       profile.count_width);
    if (!layout || layout->encoded_bytes != input.size() ||
        entry.control_element_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const std::size_t n = static_cast<std::size_t>(entry.control_element_count);
    const std::size_t h = static_cast<std::uint8_t>(profile.hash_width);
    const std::size_t o = static_cast<std::uint8_t>(entry.shape_offset_width);
    const std::size_t c = static_cast<std::uint8_t>(profile.count_width);
    const std::size_t hash_end = n * h;
    const std::size_t prefix_begin = static_cast<std::size_t>(layout->prefix_offset);
    const std::size_t prefix_end = prefix_begin + (n + 1u) * o;
    const std::size_t count_begin = static_cast<std::size_t>(layout->count_offset);
    const std::size_t count_end = count_begin + n * c;
    if (!minimizer_generation_wire_detail::all_zero(input, hash_end, prefix_begin) ||
        !minimizer_generation_wire_detail::all_zero(input, prefix_end, count_begin) ||
        !minimizer_generation_wire_detail::all_zero(input, count_end, input.size())) {
        return std::unexpected(minimizer_generation_encoding_error::reserved_nonzero);
    }
    std::vector<grouped_soa_decoded_control> controls;
    controls.reserve(n);
    std::uint64_t previous_hash = 0;
    std::uint64_t previous_prefix = 0;
    for (std::size_t i = 0; i <= n; ++i) {
        const auto prefix = minimizer_generation_wire_detail::read_shape_scalar(
            input, prefix_begin + i * o, entry.shape_offset_width);
        if (!prefix || (i == 0 && *prefix != 0) || (i != 0 && *prefix <= previous_prefix) ||
            *prefix > entry.position_element_count) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        if (i == n) {
            if (*prefix != entry.position_element_count) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            break;
        }
        const auto hash =
            minimizer_generation_wire_detail::read_shape_scalar(input, i * h, profile.hash_width);
        const auto next_prefix = minimizer_generation_wire_detail::read_shape_scalar(
            input, prefix_begin + (i + 1u) * o, entry.shape_offset_width);
        const auto count = minimizer_generation_wire_detail::read_shape_scalar(
            input, count_begin + i * c, profile.count_width);
        if (!hash || *hash > profile.max_hash || (i != 0 && *hash <= previous_hash)) {
            return std::unexpected(minimizer_generation_encoding_error::control_order);
        }
        if (!next_prefix || !count || *next_prefix <= *prefix ||
            *next_prefix > entry.position_element_count) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        const std::uint64_t delta = *next_prefix - *prefix;
        if (*count != std::min(delta, profile.max_occ_cap + 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        controls.push_back(grouped_soa_decoded_control{
            .hash = *hash,
            .position_begin = *prefix,
            .position_end = *next_prefix,
            .saturated_count = *count,
        });
        previous_hash = *hash;
        previous_prefix = *prefix;
    }
    if (controls.empty() || controls.front().hash != entry.lo_hash ||
        controls.back().hash != entry.hi_hash) {
        return std::unexpected(minimizer_generation_encoding_error::hash_range);
    }
    return controls;
}

/// Decode every grouped-SoA position to the ordinary global `(bp<<1)|strand`
/// query ABI and validate strict order independently inside every hash range.
[[nodiscard]] inline std::expected<std::vector<std::uint64_t>, minimizer_generation_encoding_error>
decode_grouped_soa_positions(std::span<const std::byte> input,
                             std::span<const grouped_soa_decoded_control> controls,
                             const grouped_soa_bucket_directory_entry& entry,
                             const minimizer_position_segment_bounds& core,
                             const grouped_soa_minimizer_profile& profile) {
    if (profile.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        input.size() != entry.position_encoded_bytes ||
        entry.position_element_count > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    const std::size_t p = static_cast<std::uint8_t>(profile.position_width);
    if (entry.position_element_count > (std::numeric_limits<std::size_t>::max)() / p ||
        input.size() != static_cast<std::size_t>(entry.position_element_count) * p) {
        return std::unexpected(minimizer_generation_encoding_error::byte_count);
    }
    std::vector<std::uint64_t> positions;
    positions.reserve(static_cast<std::size_t>(entry.position_element_count));
    for (std::size_t i = 0; i < entry.position_element_count; ++i) {
        const auto local = minimizer_generation_wire_detail::read_shape_scalar(
            input, i * p, profile.position_width);
        if (!local) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t local_bp = *local >> 1u;
        if (local_bp >= core.end_bp - core.begin_bp ||
            core.begin_bp > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t global_bp = core.begin_bp + local_bp;
        if (global_bp > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        positions.push_back((global_bp << 1u) | (*local & 1u));
    }
    for (const auto& control : controls) {
        std::optional<std::uint64_t> previous;
        for (std::uint64_t i = control.position_begin; i < control.position_end; ++i) {
            const std::uint64_t value = positions[static_cast<std::size_t>(i)];
            if (previous && value <= *previous) {
                return std::unexpected(minimizer_generation_encoding_error::position_range);
            }
            previous = value;
        }
    }
    return positions;
}

namespace minimizer_prepared_reader_detail {

/// Wire operations specialized by authenticated storage layout.
template <class p_traits> struct prepared_query_wire_ops;

/// grouped-SoA stores retained positions in one flat image and therefore does
/// not allocate an empty vector per query hash.
template <class p_hash, bool p_flat_position_image> struct prepared_query_record;

/// Query state stored beside one shared flat position image.
template <class p_hash> struct prepared_query_record<p_hash, true> {
    std::uint64_t global_occurrence_count = 0;
    std::uint64_t global_position_offset = 0;
    std::uint64_t global_positions_written = 0;
    bool positions_complete = true;
};

/// Query state with a per-query retained position vector.
template <class p_hash> struct prepared_query_record<p_hash, false> {
    p_hash hash = 0;
    std::uint64_t global_occurrence_count = 0;
    std::uint64_t global_position_offset = 0;
    std::uint64_t global_positions_written = 0;
    bool positions_complete = true;
    std::vector<std::uint64_t> positions;
};

static_assert(sizeof(prepared_query_record<std::uint64_t, true>) == 32u);
static_assert(sizeof(prepared_query_record<std::uint64_t, false>) == 64u);

/// Exact scalar owner used by the grouped-SoA prepared reader.  p_width is chosen
/// from authenticated generation/query bounds. No biological count is ever
/// capped to the runtime max-occ threshold.
enum class prepared_query_scalar_width : std::uint8_t {
    u8 = 1u,
    u16 = 2u,
    u32 = 4u,
    u64 = 8u,
};

/// Select the narrowest prepared scalar width for a known maximum.
[[nodiscard]] constexpr prepared_query_scalar_width
prepared_query_scalar_width_for_max(std::uint64_t maximum) noexcept {
    return maximum <= (std::numeric_limits<std::uint8_t>::max)() ? prepared_query_scalar_width::u8
           : maximum <= (std::numeric_limits<std::uint16_t>::max)()
               ? prepared_query_scalar_width::u16
           : maximum <= (std::numeric_limits<std::uint32_t>::max)()
               ? prepared_query_scalar_width::u32
               : prepared_query_scalar_width::u64;
}

/// Return the largest value represented by a prepared scalar width.
[[nodiscard]] constexpr std::uint64_t
prepared_query_scalar_max(prepared_query_scalar_width width) noexcept {
    switch (width) {
    case prepared_query_scalar_width::u8:
        return (std::numeric_limits<std::uint8_t>::max)();
    case prepared_query_scalar_width::u16:
        return (std::numeric_limits<std::uint16_t>::max)();
    case prepared_query_scalar_width::u32:
        return (std::numeric_limits<std::uint32_t>::max)();
    case prepared_query_scalar_width::u64:
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return 0u;
}

/// Owns one runtime-selected unsigned scalar array without widening its elements.
struct prepared_query_scalar_array {
    std::unique_ptr<std::uint8_t[]> u8;
    std::unique_ptr<std::uint16_t[]> u16;
    std::unique_ptr<std::uint32_t[]> u32;
    std::unique_ptr<std::uint64_t[]> u64;
    std::size_t count = 0u;
    prepared_query_scalar_width width = prepared_query_scalar_width::u8;

    void reset() noexcept {
        u8.reset();
        u16.reset();
        u32.reset();
        u64.reset();
        count = 0u;
        width = prepared_query_scalar_width::u8;
    }

    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept {
        return bioseq::common::saturating_mul(static_cast<std::uint64_t>(count),
                                              static_cast<std::uint8_t>(width));
    }

    void allocate(prepared_query_scalar_width wanted_width,
                  std::size_t wanted_count,
                  bool zero_initialize) {
        reset();
        width = wanted_width;
        count = wanted_count;
        if (wanted_count == 0u)
            return;
        switch (width) {
        case prepared_query_scalar_width::u8:
            u8 = std::make_unique_for_overwrite<std::uint8_t[]>(wanted_count);
            if (zero_initialize)
                std::fill_n(u8.get(), wanted_count, 0u);
            return;
        case prepared_query_scalar_width::u16:
            u16 = std::make_unique_for_overwrite<std::uint16_t[]>(wanted_count);
            if (zero_initialize)
                std::fill_n(u16.get(), wanted_count, 0u);
            return;
        case prepared_query_scalar_width::u32:
            u32 = std::make_unique_for_overwrite<std::uint32_t[]>(wanted_count);
            if (zero_initialize)
                std::fill_n(u32.get(), wanted_count, 0u);
            return;
        case prepared_query_scalar_width::u64:
            u64 = std::make_unique_for_overwrite<std::uint64_t[]>(wanted_count);
            if (zero_initialize)
                std::fill_n(u64.get(), wanted_count, 0u);
            return;
        }
    }

    [[nodiscard]] std::uint64_t get(std::size_t index) const noexcept {
        if (index >= count)
            return 0u;
        switch (width) {
        case prepared_query_scalar_width::u8:
            return u8[index];
        case prepared_query_scalar_width::u16:
            return u16[index];
        case prepared_query_scalar_width::u32:
            return u32[index];
        case prepared_query_scalar_width::u64:
            return u64[index];
        }
        return 0u;
    }

    [[nodiscard]] bool set(std::size_t index, std::uint64_t value) noexcept {
        if (index >= count || value > prepared_query_scalar_max(width)) {
            return false;
        }
        switch (width) {
        case prepared_query_scalar_width::u8:
            u8[index] = static_cast<std::uint8_t>(value);
            return true;
        case prepared_query_scalar_width::u16:
            u16[index] = static_cast<std::uint16_t>(value);
            return true;
        case prepared_query_scalar_width::u32:
            u32[index] = static_cast<std::uint32_t>(value);
            return true;
        case prepared_query_scalar_width::u64:
            u64[index] = value;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool add(std::size_t index, std::uint64_t delta) noexcept {
        if (index >= count)
            return false;
        const std::uint64_t old_value = get(index);
        const std::uint64_t maximum = prepared_query_scalar_max(width);
        return delta <= maximum - old_value && set(index, old_value + delta);
    }

    [[nodiscard]] bool increment(std::size_t index) noexcept {
        return add(index, 1u);
    }
};

template <>
struct prepared_query_wire_ops<
    codec_traits<minimizer_generation_layout::grouped_soa_segment_local>> {
    using traits = codec_traits<minimizer_generation_layout::grouped_soa_segment_local>;
    using control_type = traits::control_type;

    [[nodiscard]] static std::expected<std::vector<control_type>,
                                       minimizer_generation_encoding_error>
    decode_controls(std::span<const std::byte> bytes,
                    const grouped_soa_bucket_directory_entry& entry,
                    const grouped_soa_minimizer_profile& profile) {
        return decode_grouped_soa_controls(bytes, entry, profile);
    }

    [[nodiscard]] static std::uint64_t hash(const control_type& control) noexcept {
        return control.hash;
    }
    [[nodiscard]] static std::uint64_t position_begin(const control_type& control) noexcept {
        return control.position_begin;
    }
    [[nodiscard]] static std::uint64_t position_end(const control_type& control) noexcept {
        return control.position_end;
    }

    [[nodiscard]] static std::expected<std::uint64_t, minimizer_generation_encoding_error>
    decode_position(std::span<const std::byte> bytes,
                    const minimizer_position_segment_bounds& core,
                    const grouped_soa_minimizer_profile& profile) noexcept {
        if (bytes.size() != static_cast<std::uint8_t>(profile.position_width)) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        const auto local =
            minimizer_generation_wire_detail::read_shape_scalar(bytes, 0u, profile.position_width);
        if (!local) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t local_bp = *local >> 1u;
        if (local_bp >= core.end_bp - core.begin_bp ||
            core.begin_bp > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t global_bp = core.begin_bp + local_bp;
        if (global_bp > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return (global_bp << 1u) | (*local & 1u);
    }
};

/// The admitted grouped-SoA query protocol owns query routing, physical cursor,
/// authenticated extent session, target-global occurrence policy, retained
/// positions, and the permit covering those allocations.
template <class p_traits>
class trait_driven_prepared_query_core : protected admitted_prepared_reader_core<p_traits> {
  protected:
    using identity_core = admitted_prepared_reader_core<p_traits>;
    using wire_ops = prepared_query_wire_ops<p_traits>;
    using hash_type = typename p_traits::hash_type;

    static constexpr std::size_t no_route = (std::numeric_limits<std::size_t>::max)();

    using query_record = prepared_query_record<hash_type, p_traits::global_controls_then_positions>;

    struct dense_selected_control {
        std::size_t query_index = no_route;
        std::uint64_t position_end = 0;
    };

    // Retain every authenticated control boundary but only query routes.
    using selected_control = std::conditional_t<p_traits::global_controls_then_positions,
                                                std::uint64_t,
                                                dense_selected_control>;

    struct dense_selected_control_state {
        std::vector<dense_selected_control> controls;
    };
    struct exact_selected_control_state {
        // minimizer-fragment grouped-SoA extents remain independent.  Retain each fragment's
        // authenticated control boundaries at the narrowest width selected by
        // that fragment's position count instead of globalising every local
        // boundary into a target-wide u64 image.
        prepared_query_scalar_array position_ends;
        std::size_t control_count = 0;
    };
    using selected_control_state = std::conditional_t<p_traits::global_controls_then_positions,
                                                      exact_selected_control_state,
                                                      dense_selected_control_state>;

    static constexpr std::uint32_t no_narrow_route = (std::numeric_limits<std::uint32_t>::max)();

    struct sparse_selected_route {
        std::uint32_t control_ordinal = 0;
        std::uint32_t query_index = no_narrow_route;
    };
    static_assert(sizeof(sparse_selected_route) == 8u);

    struct no_selected_routes {};
    struct adaptive_selected_routes {
        std::unique_ptr<std::uint32_t[]> dense_routes;
        std::unique_ptr<sparse_selected_route[]> sparse_routes;
        std::size_t sparse_route_count = 0;
        std::size_t next_sparse_route = 0;
        bool uses_sparse = false;
    };
    using selected_route_state = std::conditional_t<p_traits::global_controls_then_positions,
                                                    adaptive_selected_routes,
                                                    no_selected_routes>;

    struct selected_extent {
        [[no_unique_address]] selected_control_state control_state;
        [[no_unique_address]] selected_route_state route_state;
        std::size_t next_control = 0;
        std::uint64_t next_position = 0;
        // The physical cursor consumes one control at a time.  A prior
        // position therefore belongs to the current control, not every
        // retained control record.  The validity bit resets exactly when the
        // boundary cursor advances and remains live across payload chunks.
        std::uint64_t previous_position = 0;
        bool previous_position_valid = false;
        bool controls_complete = false;
        bool positions_complete = false;
        // Exact heap owners nested below this extent. The outer extent vector
        // is accounted separately, so replacing/releasing one extent remains
        // O(1) without rescanning all selected extents.
        std::uint64_t owned_capacity_bytes = 0;
    };

    struct extent_session {
        std::size_t entry_index = no_route;
        bool positions = false;
        std::uint32_t next_chunk = 0;
        crc64_ecma182_accumulator crc{};
    };

    std::vector<hash_type> protocol_query_hashes_;
    std::vector<query_record> protocol_query_records_;
    // Search exact bucket-implied suffix keys through one admitted high-bit
    // radix directory. The full u64 image is released before payload loading.
    prepared_query_scalar_array protocol_query_suffixes_;
    std::size_t protocol_query_count_ = 0u;
    std::unique_ptr<std::uint32_t[]> protocol_query_radix_offsets_;
    std::size_t protocol_query_radix_offset_count_ = 0u;
    std::uint32_t protocol_query_radix_bits_ = 0u;
    std::uint32_t protocol_query_hash_bits_ = 0u;
    std::uint32_t protocol_query_suffix_bits_ = 0u;
    // grouped-SoA keeps exact target-global counts at the narrowest width
    // selected by the authenticated manifest position_count. Runtime max-occ is not
    // applied until every control extent has contributed. Dense offsets make
    // the hot positive lookup one direct load; UINT64_MAX means pruned.
    prepared_query_scalar_array protocol_query_exact_counts_;
    std::unique_ptr<std::uint64_t[]> protocol_query_position_offsets_;
    std::size_t protocol_query_position_offset_count_ = 0u;
    prepared_query_scalar_array protocol_query_written_;
    // The final retained position count is known before allocation, allowing
    // one exact array instead of a capacity-growing vector.
    std::unique_ptr<std::uint64_t[]> protocol_global_positions_;
    std::size_t protocol_global_position_count_ = 0u;
    std::vector<std::size_t> protocol_ordinal_to_selected_;
    std::vector<selected_extent> protocol_selected_extents_;
    std::vector<std::byte> protocol_control_scratch_;
    std::optional<extent_session> protocol_session_{};
    protocol_owned_bytes_ledger protocol_owned_bytes_{};
    std::uint64_t protocol_max_selected_control_bytes_ = 0u;
    std::uint64_t protocol_max_occ_ = 0;
    std::size_t protocol_entry_index_ = 0;
    std::size_t protocol_segment_begin_ = 0;
    std::size_t protocol_segment_end_ = 0;
    bool protocol_position_stage_ = false;
    bool protocol_global_position_storage_prepared_ = false;
    bool protocol_initialized_ = false;
    bool protocol_ready_ = false;
    bool protocol_failed_ = false;
    minimizer_generation_encoding_error protocol_failure_ =
        minimizer_generation_encoding_error::row_order;
    bioseq::parallelism::memory_admission_controller* protocol_admission_ = nullptr;
    grouped_soa_query_reader_options protocol_test_options_{};
    grouped_soa_query_admission_stats protocol_admission_failure_stats_{};

    struct protocol_memory_stats {
        std::uint64_t capacity_accounting_passes = 0;
        std::uint64_t global_position_storage_preparations = 0;
        std::uint64_t global_position_storage_allocations = 0;
        std::uint64_t position_append_growth_reservations = 0;
        std::uint64_t decoded_control_records = 0;
        std::uint64_t retained_selected_control_records = 0;
        std::uint64_t retained_selected_control_capacity_bytes = 0;
        std::uint64_t released_query_hash_capacity_bytes = 0;
        std::uint64_t query_route_radix_bucket_count = 0;
        std::uint64_t query_route_radix_nonempty_bucket_count = 0;
        std::uint64_t query_route_radix_max_bucket_size = 0;
        std::uint64_t query_route_control_lookups = 0;
        std::uint64_t query_route_hash_probes = 0;
        std::uint64_t query_route_matches = 0;
        bool saturated = false;
    };
    mutable protocol_memory_stats protocol_memory_stats_{};

    [[nodiscard]] static std::size_t
    selected_control_count_(const selected_extent& extent) noexcept {
        if constexpr (p_traits::global_controls_then_positions) {
            return extent.control_state.control_count;
        } else {
            return extent.control_state.controls.size();
        }
    }

    [[nodiscard]] static std::uint64_t selected_position_end_(const selected_extent& extent,
                                                              std::size_t control_index) noexcept {
        if constexpr (p_traits::global_controls_then_positions) {
            return extent.control_state.position_ends.get(control_index);
        } else {
            return extent.control_state.controls[control_index].position_end;
        }
    }

    [[nodiscard]] static std::uint64_t
    selected_extent_control_capacity_bytes_(const selected_extent& extent) noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = 0u;
        if constexpr (p_traits::global_controls_then_positions) {
            bytes = extent.control_state.position_ends.capacity_bytes();
            if (extent.route_state.uses_sparse) {
                bytes = saturating_add(bytes,
                                       saturating_mul(static_cast<std::uint64_t>(
                                                          extent.route_state.sparse_route_count),
                                                      sizeof(sparse_selected_route)));
            } else if (extent.route_state.dense_routes) {
                bytes = saturating_add(
                    bytes,
                    saturating_mul(static_cast<std::uint64_t>(extent.control_state.control_count),
                                   sizeof(std::uint32_t)));
            }
        } else {
            bytes =
                saturating_mul(static_cast<std::uint64_t>(extent.control_state.controls.capacity()),
                               sizeof(dense_selected_control));
        }
        return bytes;
    }

    [[nodiscard]] std::size_t protocol_query_state_count_() const noexcept {
        if constexpr (p_traits::global_controls_then_positions) {
            return protocol_query_count_;
        }
        return protocol_query_records_.size();
    }

    [[nodiscard]] std::size_t protocol_query_state_capacity_() const noexcept {
        if constexpr (p_traits::global_controls_then_positions) {
            return protocol_query_exact_counts_.count;
        }
        return protocol_query_records_.capacity();
    }

    [[nodiscard]] std::uint64_t protocol_query_state_capacity_bytes_() const noexcept {
        if constexpr (!p_traits::global_controls_then_positions) {
            return bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(protocol_query_records_.capacity()),
                sizeof(query_record));
        }
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = protocol_query_exact_counts_.capacity_bytes();
        bytes = saturating_add(
            bytes,
            saturating_mul(static_cast<std::uint64_t>(protocol_query_position_offset_count_),
                           sizeof(std::uint64_t)));
        return saturating_add(bytes, protocol_query_written_.capacity_bytes());
    }

    [[nodiscard]] std::uint64_t protocol_query_hash_capacity_bytes_() const noexcept {
        if constexpr (p_traits::global_controls_then_positions) {
            return protocol_query_suffixes_.capacity_bytes();
        }
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(protocol_query_hashes_.capacity()), sizeof(hash_type));
    }

    [[nodiscard]] std::uint64_t
    protocol_query_exact_count_(std::size_t query_index) const noexcept {
        if constexpr (p_traits::global_controls_then_positions) {
            return protocol_query_exact_counts_.get(query_index);
        }
        return query_index < protocol_query_records_.size()
                   ? protocol_query_records_[query_index].global_occurrence_count
                   : 0u;
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    add_protocol_query_exact_count_(std::size_t query_index, std::uint64_t delta) noexcept {
        if constexpr (!p_traits::global_controls_then_positions) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (delta == 0u || !protocol_query_exact_counts_.add(query_index, delta) ||
            protocol_query_exact_counts_.get(query_index) > this->manifest_.position_count) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        return {};
    }

    [[nodiscard]] bool protocol_query_retained_(std::size_t query_index) const noexcept {
        return query_index < protocol_query_position_offset_count_ &&
               protocol_query_position_offsets_ &&
               protocol_query_position_offsets_[query_index] !=
                   (std::numeric_limits<std::uint64_t>::max)();
    }

    trait_driven_prepared_query_core() = default;
    trait_driven_prepared_query_core(trait_driven_prepared_query_core&&) noexcept = default;
    trait_driven_prepared_query_core&
    operator=(trait_driven_prepared_query_core&&) noexcept = default;
    trait_driven_prepared_query_core(const trait_driven_prepared_query_core&) = delete;
    trait_driven_prepared_query_core& operator=(const trait_driven_prepared_query_core&) = delete;

    /// Walk allocation owners for protocol barriers and focused tests.
    /// Admission and reconciliation read the cached byte totals.
    [[nodiscard]] std::uint64_t protocol_owned_capacity_bytes_oracle_() const noexcept {
        if (protocol_memory_stats_.capacity_accounting_passes ==
            (std::numeric_limits<std::uint64_t>::max)()) {
            protocol_memory_stats_.saturated = true;
        } else {
            ++protocol_memory_stats_.capacity_accounting_passes;
        }
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = this->core_owned_capacity_bytes();
        const auto add = [&bytes](std::size_t capacity, std::size_t width) noexcept {
            bytes =
                saturating_add(bytes, saturating_mul(static_cast<std::uint64_t>(capacity), width));
        };
        if constexpr (p_traits::global_controls_then_positions) {
            bytes = saturating_add(bytes, protocol_query_suffixes_.capacity_bytes());
            bytes = saturating_add(bytes, protocol_query_state_capacity_bytes_());
            add(protocol_query_radix_offset_count_, sizeof(std::uint32_t));
            add(protocol_global_position_count_, sizeof(std::uint64_t));
        } else {
            add(protocol_query_hashes_.capacity(), sizeof(hash_type));
            add(protocol_query_records_.capacity(), sizeof(query_record));
            for (const query_record& record : protocol_query_records_) {
                add(record.positions.capacity(), sizeof(std::uint64_t));
            }
        }
        add(protocol_ordinal_to_selected_.capacity(), sizeof(std::size_t));
        add(protocol_selected_extents_.capacity(), sizeof(selected_extent));
        for (const selected_extent& extent : protocol_selected_extents_) {
            bytes = saturating_add(bytes, selected_extent_control_capacity_bytes_(extent));
        }
        add(protocol_control_scratch_.capacity(), sizeof(std::byte));
        return bytes;
    }

    [[nodiscard]] std::uint64_t protocol_owned_capacity_bytes() const noexcept {
        return protocol_owned_bytes_.total_bytes;
    }

    [[nodiscard]] bool replace_ledger_field_(std::uint64_t& field,
                                             std::uint64_t new_bytes) noexcept {
        const std::uint64_t old_bytes = field;
        if (!protocol_owned_bytes_.replace_checked(old_bytes, new_bytes)) {
            return false;
        }
        field = new_bytes;
        return true;
    }

    [[nodiscard]] bool replace_selected_extent_owner_(selected_extent& extent,
                                                      std::uint64_t new_bytes) noexcept {
        if (extent.owned_capacity_bytes > protocol_owned_bytes_.selected_extent_owner_bytes) {
            return false;
        }
        const std::uint64_t new_total =
            protocol_owned_bytes_.selected_extent_owner_bytes - extent.owned_capacity_bytes;
        if (new_bytes > (std::numeric_limits<std::uint64_t>::max)() - new_total) {
            return false;
        }
        if (!replace_ledger_field_(protocol_owned_bytes_.selected_extent_owner_bytes,
                                   new_total + new_bytes)) {
            return false;
        }
        extent.owned_capacity_bytes = new_bytes;
        return true;
    }

    [[nodiscard]] bool protocol_ledger_matches_oracle_() const noexcept {
        return protocol_owned_bytes_.field_sum_holds() &&
               protocol_owned_capacity_bytes_oracle_() == protocol_owned_capacity_bytes();
    }

    class permit_growth_guard {
      private:
        trait_driven_prepared_query_core* owner_ = nullptr;
        bool committed_ = false;

      public:
        explicit permit_growth_guard(trait_driven_prepared_query_core& owner) noexcept
            : owner_(&owner) {}
        permit_growth_guard(const permit_growth_guard&) = delete;
        permit_growth_guard& operator=(const permit_growth_guard&) = delete;
        ~permit_growth_guard() {
            if (!committed_ && owner_ != nullptr && owner_->permit_) {
                // A failed allocation/reconciliation cannot leave newly
                // grown vectors alive under the recorded reservation.
                // Every protocol operation is fail-closed, so destroy all
                // covered storage before releasing the permit.
                owner_->protocol_ready_ = false;
                owner_->protocol_release_noexcept();
            }
        }
        [[nodiscard]] bool
        grow(std::uint64_t projected,
             bioseq::parallelism::memory_admission_failure* failure = nullptr) noexcept {
            return projected != (std::numeric_limits<std::uint64_t>::max)() &&
                   owner_->permit_.grow_to(projected, failure);
        }
        [[nodiscard]] bool reconcile() noexcept {
            const std::uint64_t actual = owner_->protocol_owned_capacity_bytes();
            if (actual == (std::numeric_limits<std::uint64_t>::max)() ||
                actual > owner_->permit_.bytes()) {
                return false;
            }
            owner_->permit_.shrink_to(actual);
            committed_ = true;
            return true;
        }
    };

    [[nodiscard]] bool release_control_scratch_noexcept_() noexcept {
        {
            std::vector<std::byte> empty;
            protocol_control_scratch_.swap(empty);
        }
        return replace_ledger_field_(protocol_owned_bytes_.control_scratch_bytes, 0u);
    }

    void protocol_release_noexcept() noexcept {
        protocol_session_.reset();
        {
            std::vector<std::byte> empty;
            protocol_control_scratch_.swap(empty);
        }
        std::vector<selected_extent>{}.swap(protocol_selected_extents_);
        std::vector<std::size_t>{}.swap(protocol_ordinal_to_selected_);
        protocol_global_positions_.reset();
        protocol_global_position_count_ = 0u;
        protocol_query_suffixes_.reset();
        protocol_query_count_ = 0u;
        protocol_query_radix_offsets_.reset();
        protocol_query_radix_offset_count_ = 0u;
        protocol_query_radix_bits_ = 0u;
        protocol_query_hash_bits_ = 0u;
        protocol_query_suffix_bits_ = 0u;
        protocol_query_exact_counts_.reset();
        protocol_query_position_offsets_.reset();
        protocol_query_position_offset_count_ = 0u;
        protocol_query_written_.reset();
        std::vector<query_record>{}.swap(protocol_query_records_);
        std::vector<hash_type>{}.swap(protocol_query_hashes_);
        protocol_max_selected_control_bytes_ = 0u;
        protocol_global_position_storage_prepared_ = false;
        this->release_core_noexcept();
        protocol_owned_bytes_.reset();
        if (protocol_owned_capacity_bytes_oracle_() != 0u) {
            protocol_memory_stats_.saturated = true;
        }
    }

    template <class p_value>
    [[nodiscard]] std::expected<p_value, minimizer_generation_encoding_error>
    protocol_poison(minimizer_generation_encoding_error error) noexcept {
        if (!protocol_failed_) {
            protocol_failed_ = true;
            protocol_ready_ = false;
            protocol_failure_ = error;
            protocol_release_noexcept();
        }
        return std::unexpected(protocol_failure_);
    }

    void mark_protocol_moved_from() noexcept {
        protocol_session_.reset();
        std::vector<std::byte>{}.swap(protocol_control_scratch_);
        protocol_global_positions_.reset();
        protocol_global_position_count_ = 0u;
        protocol_query_suffixes_.reset();
        protocol_query_count_ = 0u;
        protocol_query_radix_offsets_.reset();
        protocol_query_radix_offset_count_ = 0u;
        protocol_query_radix_bits_ = 0u;
        protocol_query_hash_bits_ = 0u;
        protocol_query_suffix_bits_ = 0u;
        protocol_query_exact_counts_.reset();
        protocol_query_position_offsets_.reset();
        protocol_query_position_offset_count_ = 0u;
        protocol_query_written_.reset();
        protocol_max_selected_control_bytes_ = 0u;
        protocol_owned_bytes_.reset();
        protocol_global_position_storage_prepared_ = false;
        protocol_initialized_ = true;
        protocol_ready_ = false;
        protocol_failed_ = true;
        protocol_failure_ = minimizer_generation_encoding_error::row_order;
    }

    void increment_memory_stats_(std::uint64_t& counter) const noexcept {
        if (counter == (std::numeric_limits<std::uint64_t>::max)()) {
            protocol_memory_stats_.saturated = true;
        } else {
            ++counter;
        }
    }

    void add_memory_stats_(std::uint64_t& counter, std::uint64_t delta) const noexcept {
        if (counter > (std::numeric_limits<std::uint64_t>::max)() - delta) {
            counter = (std::numeric_limits<std::uint64_t>::max)();
            protocol_memory_stats_.saturated = true;
        } else {
            counter += delta;
        }
    }

    [[nodiscard]] bool
    inject_admission_failure_(grouped_soa_query_admission_stage substage) noexcept {
        if constexpr (!p_traits::global_controls_then_positions) {
            return false;
        }
        if (protocol_test_options_.fail_admission_substage != substage) {
            return false;
        }
        protocol_test_options_.fail_admission_substage = grouped_soa_query_admission_stage::none;
        return true;
    }

    void capture_admission_failure_(
        grouped_soa_query_admission_stage substage,
        std::uint64_t projected_bytes,
        std::uint64_t retained_position_count = 0u,
        std::uint64_t retained_position_bytes = 0u,
        std::uint64_t allocation_headroom_bytes = 0u,
        const bioseq::parallelism::memory_admission_failure* controller_failure = nullptr,
        bool injected = false) noexcept {
        if constexpr (!p_traits::global_controls_then_positions) {
            return;
        }
        if (protocol_admission_failure_stats_.substage != grouped_soa_query_admission_stage::none) {
            return;
        }
        using bioseq::common::saturating_mul;
        const std::uint64_t owned_capacity_bytes = protocol_owned_capacity_bytes();
        const std::uint64_t permit_bytes = this->permit_.bytes();
        grouped_soa_query_admission_stats stats{
            .substage = substage,
            .retained_position_count = retained_position_count,
            .retained_position_bytes = retained_position_bytes,
            .allocation_headroom_bytes = allocation_headroom_bytes,
            .projected_bytes = projected_bytes,
            .requested_growth_bytes =
                projected_bytes > permit_bytes ? projected_bytes - permit_bytes : 0u,
            .owned_capacity_bytes = owned_capacity_bytes,
            .permit_bytes = permit_bytes,
            .query_record_count = static_cast<std::uint64_t>(protocol_query_state_count_()),
            .query_record_capacity = static_cast<std::uint64_t>(protocol_query_state_capacity_()),
            .query_hash_count = static_cast<std::uint64_t>(p_traits::global_controls_then_positions
                                                               ? protocol_query_suffixes_.count
                                                               : protocol_query_hashes_.size()),
            .query_hash_capacity = static_cast<std::uint64_t>(
                p_traits::global_controls_then_positions ? protocol_query_suffixes_.count
                                                         : protocol_query_hashes_.capacity()),
            .selected_extent_count = static_cast<std::uint64_t>(protocol_selected_extents_.size()),
            .selected_extent_capacity =
                static_cast<std::uint64_t>(protocol_selected_extents_.capacity()),
            .selected_control_capacity_bytes = protocol_owned_bytes_.selected_extent_owner_bytes,
            .control_payload_capacity_bytes = protocol_owned_bytes_.control_scratch_bytes,
            .releasable_protocol_query_hash_bytes =
                !p_traits::global_controls_then_positions && protocol_initialized_ &&
                        protocol_query_records_.size() == protocol_query_hashes_.size()
                    ? saturating_mul(static_cast<std::uint64_t>(protocol_query_hashes_.capacity()),
                                     sizeof(hash_type))
                    : 0u,
            .injected_failure = injected,
        };
        if (controller_failure != nullptr && controller_failure->has_failure()) {
            stats.controller_reason = controller_failure->reason;
            stats.requested_growth_bytes = controller_failure->requested_bytes;
            stats.controller_reserved_bytes = controller_failure->reserved_bytes;
            stats.controller_budget_ceiling_bytes = controller_failure->budget_ceiling_bytes;
            stats.controller_allocatable_after_reservations_bytes =
                controller_failure->allocatable_after_reservations_bytes;
            stats.process_footprint_bytes = controller_failure->process_footprint_bytes;
            stats.system_available_bytes = controller_failure->system_available_bytes;
            stats.system_total_bytes = controller_failure->system_total_bytes;
            stats.process_soft_cap_bytes = controller_failure->process_soft_cap_bytes;
            stats.controller_failure_snapshot_present = true;
        } else if (protocol_admission_ != nullptr) {
            const auto snapshot = protocol_admission_->diagnostic_snapshot();
            stats.controller_reserved_bytes = snapshot.reserved_bytes;
            stats.controller_budget_ceiling_bytes = snapshot.budget_ceiling_bytes;
            stats.controller_allocatable_after_reservations_bytes =
                snapshot.allocatable_after_reservations_bytes;
            stats.process_footprint_bytes = snapshot.process_footprint_bytes;
            stats.system_available_bytes = snapshot.system_available_bytes;
            stats.system_total_bytes = snapshot.system_total_bytes;
            stats.process_soft_cap_bytes = snapshot.process_soft_cap_bytes;
        }
        protocol_admission_failure_stats_ = stats;
    }

    /// grouped-SoA persists all controls before all positions.  At the stage
    /// boundary every selected hash's target-global occurrence count is final,
    /// so admit and allocate the complete retained-position image once.  This
    /// is intentionally one exact flat array: a query union with Q mostly-unique
    /// hashes must not perform Q live Mach/host memory probes, Q heap
    /// allocations, or Q full ownership walks before it can read positions.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    prepare_global_position_storage() {
        if constexpr (!p_traits::global_controls_then_positions) {
            return {};
        }
        if (protocol_global_position_storage_prepared_ || !protocol_position_stage_ ||
            protocol_entry_index_ != 0u || protocol_session_ || protocol_global_positions_ ||
            protocol_global_position_count_ != 0u || protocol_query_position_offsets_ ||
            protocol_query_position_offset_count_ != 0u || protocol_query_written_.count != 0u) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        increment_memory_stats_(protocol_memory_stats_.global_position_storage_preparations);

        std::uint64_t retained_count = 0;
        for (std::size_t query_index = 0u; query_index < protocol_query_count_; ++query_index) {
            const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
            if (exact_count > this->manifest_.position_count) {
                return std::unexpected(minimizer_generation_encoding_error::element_count);
            }
            if (exact_count == 0u || exact_count > protocol_max_occ_) {
                continue;
            }
            if (exact_count > (std::numeric_limits<std::uint64_t>::max)() - retained_count) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            retained_count += exact_count;
        }
        if (retained_count > (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint64_t)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        permit_growth_guard guard{*this};
        const auto planned =
            plan_grouped_soa_position_image(retained_count, protocol_owned_capacity_bytes());
        if (!planned)
            return std::unexpected(planned.error());
        const std::uint64_t retained_bytes = planned->retained_position_bytes;
        const std::uint64_t headroom = planned->allocation_headroom_bytes;
        const prepared_query_scalar_width written_width =
            prepared_query_scalar_width_for_max(protocol_max_occ_);
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t compact_final_bytes = saturating_mul(
            static_cast<std::uint64_t>(protocol_query_count_), sizeof(std::uint64_t));
        compact_final_bytes =
            saturating_add(compact_final_bytes,
                           saturating_mul(static_cast<std::uint64_t>(protocol_query_count_),
                                          static_cast<std::uint8_t>(written_width)));
        const std::uint64_t projected =
            saturating_add(planned->projected_bytes, compact_final_bytes);
        if (compact_final_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::position_image_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::position_image_reservation,
                projected,
                retained_count,
                retained_bytes,
                headroom,
                nullptr,
                true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure position_failure;
        if (!guard.grow(projected, &position_failure)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::position_image_reservation,
                projected,
                retained_count,
                retained_bytes,
                headroom,
                &position_failure);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (retained_count != 0u &&
            inject_admission_failure_(
                grouped_soa_query_admission_stage::position_image_allocation)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::position_image_allocation,
                                       projected,
                                       retained_count,
                                       retained_bytes,
                                       headroom,
                                       nullptr,
                                       true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        try {
            std::unique_ptr<std::uint64_t[]> position_offsets;
            prepared_query_scalar_array written;
            std::unique_ptr<std::uint64_t[]> global_positions;
            if (protocol_query_count_ != 0u) {
                position_offsets =
                    std::make_unique_for_overwrite<std::uint64_t[]>(protocol_query_count_);
                std::fill_n(position_offsets.get(),
                            protocol_query_count_,
                            (std::numeric_limits<std::uint64_t>::max)());
                written.allocate(written_width, protocol_query_count_, true);
            }
            if (retained_count != 0u) {
                global_positions = std::make_unique_for_overwrite<std::uint64_t[]>(
                    static_cast<std::size_t>(retained_count));
            }
            std::uint64_t retained_offset = 0u;
            for (std::size_t query_index = 0u; query_index < protocol_query_count_; ++query_index) {
                const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
                if (exact_count == 0u || exact_count > protocol_max_occ_) {
                    continue;
                }
                position_offsets[query_index] = retained_offset;
                retained_offset += exact_count;
            }
            if (retained_offset != retained_count) {
                return std::unexpected(minimizer_generation_encoding_error::element_count);
            }
            protocol_query_position_offsets_ = std::move(position_offsets);
            protocol_query_position_offset_count_ = protocol_query_count_;
            protocol_query_written_ = std::move(written);
            protocol_global_positions_ = std::move(global_positions);
            protocol_global_position_count_ = static_cast<std::size_t>(retained_count);
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(grouped_soa_query_admission_stage::position_image_allocation,
                                       projected,
                                       retained_count,
                                       retained_bytes,
                                       headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!replace_ledger_field_(protocol_owned_bytes_.global_position_bytes, retained_bytes) ||
            !replace_ledger_field_(protocol_owned_bytes_.query_record_bytes,
                                   protocol_query_state_capacity_bytes_())) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (retained_count != 0u) {
            increment_memory_stats_(protocol_memory_stats_.global_position_storage_allocations);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::position_image_reconcile)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::position_image_reconcile,
                                       projected,
                                       retained_count,
                                       retained_bytes,
                                       headroom,
                                       nullptr,
                                       true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!guard.reconcile()) {
            capture_admission_failure_(grouped_soa_query_admission_stage::position_image_reconcile,
                                       projected,
                                       retained_count,
                                       retained_bytes,
                                       headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!protocol_ledger_matches_oracle_()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        protocol_global_position_storage_prepared_ = true;
        return {};
    }

    static constexpr std::size_t query_radix_target_mean_ = 4u;

    struct query_radix_plan {
        std::uint32_t hash_bits = 0u;
        std::uint32_t radix_bits = 0u;
        std::uint32_t suffix_bits = 0u;
        prepared_query_scalar_width suffix_width = prepared_query_scalar_width::u8;
        std::size_t bucket_count = 1u;
        std::size_t offset_count = 2u;
    };

    /// Size the high-bit directory for a tiny exact point-lookup bucket. The
    /// high bits are implicit in the bucket and only its low suffix is kept.
    [[nodiscard]] static std::expected<query_radix_plan, minimizer_generation_encoding_error>
    plan_query_radix_(std::size_t query_count, std::uint64_t max_hash) noexcept {
        const std::uint32_t hash_bits = static_cast<std::uint32_t>(std::bit_width(max_hash));
        const std::size_t required_buckets =
            query_count / query_radix_target_mean_ +
            (query_count % query_radix_target_mean_ != 0u ? 1u : 0u);
        const std::uint32_t requested_bits =
            required_buckets <= 1u ? 0u
                                   : static_cast<std::uint32_t>(std::bit_width(
                                         static_cast<std::uint64_t>(required_buckets - 1u)));
        const std::uint32_t radix_bits = (std::min)(hash_bits, requested_bits);
        if (radix_bits >= std::numeric_limits<std::size_t>::digits) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::size_t bucket_count = std::size_t{1u} << radix_bits;
        if (bucket_count == (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint32_t suffix_bits = hash_bits - radix_bits;
        const std::uint64_t maximum_suffix =
            suffix_bits == 0u ? 0u
            : suffix_bits >= std::numeric_limits<std::uint64_t>::digits
                ? (std::numeric_limits<std::uint64_t>::max)()
                : (std::uint64_t{1u} << suffix_bits) - 1u;
        return query_radix_plan{
            .hash_bits = hash_bits,
            .radix_bits = radix_bits,
            .suffix_bits = suffix_bits,
            .suffix_width = prepared_query_scalar_width_for_max(maximum_suffix),
            .bucket_count = bucket_count,
            .offset_count = bucket_count + 1u,
        };
    }

    [[nodiscard]] static std::size_t query_radix_bucket_(hash_type hash,
                                                         std::uint32_t hash_bits,
                                                         std::uint32_t radix_bits) noexcept {
        if (radix_bits == 0u)
            return 0u;
        return static_cast<std::size_t>(static_cast<std::uint64_t>(hash) >>
                                        (hash_bits - radix_bits));
    }

    [[nodiscard]] static std::uint64_t query_radix_suffix_(hash_type hash,
                                                           std::uint32_t suffix_bits) noexcept {
        if (suffix_bits == 0u)
            return 0u;
        const std::uint64_t value = static_cast<std::uint64_t>(hash);
        if (suffix_bits >= std::numeric_limits<std::uint64_t>::digits) {
            return value;
        }
        return value & ((std::uint64_t{1u} << suffix_bits) - 1u);
    }

    [[nodiscard]] std::size_t
    query_index_for_hash_impl_(std::uint64_t hash, std::uint32_t* hash_probes) const noexcept {
        if (hash_probes != nullptr)
            *hash_probes = 0u;
        if (hash > (std::numeric_limits<hash_type>::max)() || hash > this->profile_.max_hash) {
            return no_route;
        }
        const hash_type narrowed = static_cast<hash_type>(hash);
        if constexpr (p_traits::global_controls_then_positions) {
            if (!protocol_query_radix_offsets_ || protocol_query_radix_offset_count_ < 2u ||
                protocol_query_suffixes_.count != protocol_query_count_ ||
                protocol_query_exact_counts_.count != protocol_query_count_) {
                return no_route;
            }
            const std::size_t bucket = query_radix_bucket_(
                narrowed, protocol_query_hash_bits_, protocol_query_radix_bits_);
            if (bucket + 1u >= protocol_query_radix_offset_count_) {
                return no_route;
            }
            std::size_t first = protocol_query_radix_offsets_[bucket];
            const std::size_t end = protocol_query_radix_offsets_[bucket + 1u];
            if (first > end || end > protocol_query_count_) {
                return no_route;
            }
            const std::uint64_t wanted = query_radix_suffix_(narrowed, protocol_query_suffix_bits_);
            const auto search =
                [&first, &end, &wanted, &hash_probes](const auto* suffixes) noexcept {
                    std::size_t lower = first;
                    std::size_t upper = end;
                    while (lower < upper) {
                        const std::size_t middle = lower + (upper - lower) / 2u;
                        if (hash_probes != nullptr)
                            ++*hash_probes;
                        if (static_cast<std::uint64_t>(suffixes[middle]) < wanted) {
                            lower = middle + 1u;
                        } else {
                            upper = middle;
                        }
                    }
                    if (lower < end) {
                        if (hash_probes != nullptr)
                            ++*hash_probes;
                        if (static_cast<std::uint64_t>(suffixes[lower]) == wanted) {
                            return lower;
                        }
                    }
                    return no_route;
                };
            switch (protocol_query_suffixes_.width) {
            case prepared_query_scalar_width::u8:
                return search(protocol_query_suffixes_.u8.get());
            case prepared_query_scalar_width::u16:
                return search(protocol_query_suffixes_.u16.get());
            case prepared_query_scalar_width::u32:
                return search(protocol_query_suffixes_.u32.get());
            case prepared_query_scalar_width::u64:
                return search(protocol_query_suffixes_.u64.get());
            }
            return no_route;
        } else {
            const auto found = std::lower_bound(
                protocol_query_hashes_.begin(), protocol_query_hashes_.end(), narrowed);
            return found != protocol_query_hashes_.end() && *found == narrowed
                       ? static_cast<std::size_t>(found - protocol_query_hashes_.begin())
                       : no_route;
        }
    }

    [[nodiscard]] std::size_t query_index_for_hash(std::uint64_t hash) const noexcept {
        return query_index_for_hash_impl_(hash, nullptr);
    }

    [[nodiscard]] std::size_t
    query_index_for_control_hash(std::uint64_t hash, std::uint32_t& hash_probes) const noexcept {
        return query_index_for_hash_impl_(hash, &hash_probes);
    }

    [[nodiscard]] bool
    query_intersects_entry(const grouped_soa_bucket_directory_entry& entry) const noexcept {
        const auto found = std::lower_bound(protocol_query_hashes_.begin(),
                                            protocol_query_hashes_.end(),
                                            static_cast<hash_type>(entry.lo_hash));
        return found != protocol_query_hashes_.end() &&
               static_cast<std::uint64_t>(*found) <= entry.hi_hash;
    }

    void initialize_physical_cursor() noexcept {
        protocol_entry_index_ = 0;
        protocol_position_stage_ = false;
        protocol_segment_begin_ = 0;
        protocol_segment_end_ = 0;
        const auto entries = this->directory_.entries();
        if constexpr (!p_traits::global_controls_then_positions) {
            if (!entries.empty()) {
                protocol_segment_end_ = 1u;
                while (protocol_segment_end_ < entries.size() &&
                       entries[protocol_segment_end_].position_segment ==
                           entries[0].position_segment) {
                    ++protocol_segment_end_;
                }
            }
        }
    }

    void advance_physical_cursor() noexcept {
        const auto entries = this->directory_.entries();
        ++protocol_entry_index_;
        if constexpr (p_traits::global_controls_then_positions) {
            if (protocol_entry_index_ == entries.size() && !protocol_position_stage_) {
                protocol_position_stage_ = true;
                protocol_entry_index_ = 0;
            } else if (protocol_entry_index_ == entries.size()) {
                protocol_position_stage_ = false;
            }
            return;
        }
        if (protocol_entry_index_ != protocol_segment_end_)
            return;
        if (!protocol_position_stage_) {
            protocol_position_stage_ = true;
            protocol_entry_index_ = protocol_segment_begin_;
            return;
        }
        protocol_position_stage_ = false;
        protocol_segment_begin_ = protocol_segment_end_;
        protocol_entry_index_ = protocol_segment_begin_;
        if (protocol_segment_begin_ >= entries.size())
            return;
        protocol_segment_end_ = protocol_segment_begin_ + 1u;
        while (protocol_segment_end_ < entries.size() &&
               entries[protocol_segment_end_].position_segment ==
                   entries[protocol_segment_begin_].position_segment) {
            ++protocol_segment_end_;
        }
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    initialize_identity_core(bioseq::parallelism::memory_admission_controller& admission,
                             const minimizer_generation_head& head,
                             const minimizer_generation_manifest& manifest,
                             const expected_minimizer_generation_identity& expected,
                             const grouped_soa_minimizer_profile& profile,
                             std::span<const std::byte> encoded_directory,
                             std::uint64_t additional_logical_bytes = 0) {
        return identity_core::begin_admitted_core(static_cast<identity_core&>(*this),
                                                  admission,
                                                  head,
                                                  manifest,
                                                  expected,
                                                  profile,
                                                  encoded_directory,
                                                  additional_logical_bytes);
    }

    [[nodiscard]] static constexpr std::uint64_t query_hash_value_(std::uint64_t hash) noexcept {
        return hash;
    }

    [[nodiscard]] static std::uint64_t
    query_hash_value_(const minimap_sketch_data& minimizer) noexcept {
        return minimizer.kmer();
    }

    template <class p_query_value>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    initialize_query_protocol(bioseq::parallelism::memory_admission_controller& admission,
                              const minimizer_generation_head& head,
                              const minimizer_generation_manifest& manifest,
                              const expected_minimizer_generation_identity& expected,
                              const grouped_soa_minimizer_profile& profile,
                              std::span<const std::byte> encoded_directory,
                              std::span<const p_query_value> query_hashes,
                              std::uint64_t max_occ,
                              grouped_soa_query_reader_options test_options = {},
                              bool query_hashes_are_sorted_unique = false) {
        protocol_admission_ = &admission;
        if constexpr (p_traits::global_controls_then_positions) {
            protocol_test_options_ = test_options;
        }
        if (query_hashes.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(hash_type)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        std::uint64_t previous_hash = 0u;
        bool has_previous_hash = false;
        for (const p_query_value& query_value : query_hashes) {
            const std::uint64_t hash = query_hash_value_(query_value);
            if (hash > profile.max_hash || hash > (std::numeric_limits<hash_type>::max)()) {
                return std::unexpected(minimizer_generation_encoding_error::hash_range);
            }
            if (query_hashes_are_sorted_unique && has_previous_hash && hash <= previous_hash) {
                return std::unexpected(minimizer_generation_encoding_error::row_order);
            }
            previous_hash = hash;
            has_previous_hash = true;
        }
        const std::uint64_t query_bytes =
            bioseq::common::saturating_mul(query_hashes.size(), sizeof(hash_type));
        if (query_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::identity_and_directory_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::identity_and_directory_reservation,
                0u,
                0u,
                0u,
                0u,
                nullptr,
                true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure core_failure;
        if (auto admitted = identity_core::begin_admitted_core(static_cast<identity_core&>(*this),
                                                               admission,
                                                               head,
                                                               manifest,
                                                               expected,
                                                               profile,
                                                               encoded_directory,
                                                               query_bytes,
                                                               &core_failure);
            !admitted) {
            if (admitted.error() == minimizer_generation_encoding_error::admission) {
                capture_admission_failure_(
                    grouped_soa_query_admission_stage::identity_and_directory_reservation,
                    core_failure.requested_bytes,
                    0u,
                    0u,
                    0u,
                    &core_failure);
            }
            return std::unexpected(admitted.error());
        }
        if (!replace_ledger_field_(protocol_owned_bytes_.directory_bytes,
                                   this->core_owned_capacity_bytes())) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        protocol_max_occ_ = max_occ;

        permit_growth_guard guard{*this};
        std::uint64_t projected = bioseq::common::saturating_add(
            protocol_owned_capacity_bytes(),
            bioseq::common::saturating_mul(
                query_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor));
        if (projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(grouped_soa_query_admission_stage::query_hash_reservation)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_reservation,
                                       projected,
                                       0u,
                                       0u,
                                       query_bytes,
                                       nullptr,
                                       true);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure query_hash_failure;
        if (!guard.grow(projected, &query_hash_failure)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_reservation,
                                       projected,
                                       0u,
                                       0u,
                                       query_bytes,
                                       &query_hash_failure);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        if (inject_admission_failure_(grouped_soa_query_admission_stage::query_hash_allocation)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_allocation,
                                       projected,
                                       0u,
                                       0u,
                                       query_bytes,
                                       nullptr,
                                       true);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        try {
            protocol_query_hashes_.reserve(query_hashes.size());
            for (const p_query_value& query_value : query_hashes) {
                const std::uint64_t hash = query_hash_value_(query_value);
                protocol_query_hashes_.push_back(static_cast<hash_type>(hash));
            }
            if (!query_hashes_are_sorted_unique) {
                std::ranges::sort(protocol_query_hashes_);
                protocol_query_hashes_.erase(
                    std::unique(protocol_query_hashes_.begin(), protocol_query_hashes_.end()),
                    protocol_query_hashes_.end());
            }
            if constexpr (p_traits::global_controls_then_positions) {
                // The caller may provide duplicate hashes. Canonicalize into
                // a right-sized temporary image used to build suffixes and
                // select extents; the 2x reservation covers both vectors at
                // this peak, and the full image is released before payload.
                if (!query_hashes_are_sorted_unique) {
                    std::vector<hash_type> canonical;
                    canonical.reserve(protocol_query_hashes_.size());
                    canonical.assign(protocol_query_hashes_.begin(), protocol_query_hashes_.end());
                    protocol_query_hashes_.swap(canonical);
                }
            }
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_allocation,
                                       projected,
                                       0u,
                                       0u,
                                       query_bytes);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t actual_query_hash_bytes =
            bioseq::common::saturating_mul(protocol_query_hashes_.capacity(), sizeof(hash_type));
        if (actual_query_hash_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            !replace_ledger_field_(protocol_owned_bytes_.query_hash_bytes,
                                   actual_query_hash_bytes)) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(grouped_soa_query_admission_stage::query_hash_reconcile)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_reconcile,
                                       projected,
                                       0u,
                                       0u,
                                       query_bytes,
                                       nullptr,
                                       true);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        if (!guard.reconcile()) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_hash_reconcile,
                                       projected,
                                       0u,
                                       0u,
                                       query_bytes);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        if constexpr (p_traits::global_controls_then_positions) {
            // UINT32_MAX remains the dense no-route sentinel.  A grouped-SoA
            // query image must therefore leave it unavailable as an index.
            if (protocol_query_hashes_.size() >= no_narrow_route) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }

        query_radix_plan radix_plan{};
        prepared_query_scalar_width query_count_width = prepared_query_scalar_width::u8;
        if constexpr (p_traits::global_controls_then_positions) {
            const auto planned =
                plan_query_radix_(protocol_query_hashes_.size(), this->profile_.max_hash);
            if (!planned)
                return protocol_poison<void>(planned.error());
            radix_plan = *planned;
            query_count_width = prepared_query_scalar_width_for_max(this->manifest_.position_count);
        }

        const auto entries = this->directory_.entries();
        std::size_t selected_count = 0;
        for (const auto& entry : entries) {
            selected_count += query_intersects_entry(entry) ? 1u : 0u;
        }
        std::uint64_t route_vector_bytes = 0;
        route_vector_bytes = bioseq::common::saturating_add(
            route_vector_bytes,
            bioseq::common::saturating_mul(
                protocol_query_hashes_.size(),
                p_traits::global_controls_then_positions
                    ? static_cast<std::size_t>(static_cast<std::uint8_t>(radix_plan.suffix_width) +
                                               static_cast<std::uint8_t>(query_count_width))
                    : sizeof(query_record)));
        route_vector_bytes = bioseq::common::saturating_add(
            route_vector_bytes,
            bioseq::common::saturating_mul(entries.size(), sizeof(std::size_t)));
        route_vector_bytes = bioseq::common::saturating_add(
            route_vector_bytes,
            bioseq::common::saturating_mul(selected_count, sizeof(selected_extent)));
        const std::uint64_t query_radix_bytes =
            p_traits::global_controls_then_positions
                ? bioseq::common::saturating_mul(radix_plan.offset_count, sizeof(std::uint32_t))
                : 0u;
        const std::uint64_t route_bytes =
            bioseq::common::saturating_add(route_vector_bytes, query_radix_bytes);
        permit_growth_guard route_guard{*this};
        projected = bioseq::common::saturating_add(
            protocol_owned_capacity_bytes(),
            bioseq::common::saturating_add(
                bioseq::common::saturating_mul(
                    route_vector_bytes,
                    bioseq::parallelism::k_vector_preallocation_headroom_factor),
                query_radix_bytes));
        if (route_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(grouped_soa_query_admission_stage::query_route_reservation)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_route_reservation,
                                       projected,
                                       0u,
                                       0u,
                                       route_bytes,
                                       nullptr,
                                       true);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure route_failure;
        if (!route_guard.grow(projected, &route_failure)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_route_reservation,
                                       projected,
                                       0u,
                                       0u,
                                       route_bytes,
                                       &route_failure);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        if (inject_admission_failure_(grouped_soa_query_admission_stage::query_route_allocation)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_route_allocation,
                                       projected,
                                       0u,
                                       0u,
                                       route_bytes,
                                       nullptr,
                                       true);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        std::uint64_t released_full_query_hash_bytes = 0u;
        try {
            if constexpr (p_traits::global_controls_then_positions) {
                std::unique_ptr<std::uint32_t[]> radix_offsets;
                prepared_query_scalar_array suffixes;
                prepared_query_scalar_array exact_counts;
                radix_offsets =
                    std::make_unique_for_overwrite<std::uint32_t[]>(radix_plan.offset_count);
                suffixes.allocate(radix_plan.suffix_width, protocol_query_hashes_.size(), false);
                exact_counts.allocate(query_count_width, protocol_query_hashes_.size(), true);
                std::size_t query_index = 0u;
                std::uint64_t nonempty_buckets = 0u;
                std::uint64_t max_bucket_size = 0u;
                for (std::size_t bucket = 0u; bucket < radix_plan.bucket_count; ++bucket) {
                    radix_offsets[bucket] = static_cast<std::uint32_t>(query_index);
                    const std::size_t bucket_begin = query_index;
                    while (query_index < protocol_query_hashes_.size() &&
                           query_radix_bucket_(protocol_query_hashes_[query_index],
                                               radix_plan.hash_bits,
                                               radix_plan.radix_bits) == bucket) {
                        if (!suffixes.set(query_index,
                                          query_radix_suffix_(protocol_query_hashes_[query_index],
                                                              radix_plan.suffix_bits))) {
                            return protocol_poison<void>(
                                minimizer_generation_encoding_error::hash_range);
                        }
                        ++query_index;
                    }
                    const std::uint64_t bucket_size =
                        static_cast<std::uint64_t>(query_index - bucket_begin);
                    nonempty_buckets += bucket_size != 0u ? 1u : 0u;
                    max_bucket_size = (std::max)(max_bucket_size, bucket_size);
                }
                if (query_index != protocol_query_hashes_.size()) {
                    return protocol_poison<void>(minimizer_generation_encoding_error::hash_range);
                }
                radix_offsets[radix_plan.bucket_count] = static_cast<std::uint32_t>(query_index);
                protocol_query_count_ = protocol_query_hashes_.size();
                protocol_query_suffixes_ = std::move(suffixes);
                protocol_query_exact_counts_ = std::move(exact_counts);
                protocol_query_radix_offsets_ = std::move(radix_offsets);
                protocol_query_radix_offset_count_ = radix_plan.offset_count;
                protocol_query_radix_bits_ = radix_plan.radix_bits;
                protocol_query_hash_bits_ = radix_plan.hash_bits;
                protocol_query_suffix_bits_ = radix_plan.suffix_bits;
                protocol_memory_stats_.query_route_radix_bucket_count = radix_plan.bucket_count;
                protocol_memory_stats_.query_route_radix_nonempty_bucket_count = nonempty_buckets;
                protocol_memory_stats_.query_route_radix_max_bucket_size = max_bucket_size;
            } else {
                protocol_query_records_.reserve(protocol_query_hashes_.size());
                for (const hash_type hash : protocol_query_hashes_) {
                    protocol_query_records_.push_back(query_record{.hash = hash});
                }
            }
            protocol_ordinal_to_selected_.assign(entries.size(), no_route);
            protocol_selected_extents_.reserve(selected_count);
            for (const auto& entry : entries) {
                if (!query_intersects_entry(entry))
                    continue;
                protocol_ordinal_to_selected_[entry.partition_id] =
                    protocol_selected_extents_.size();
                protocol_selected_extents_.push_back(selected_extent{});
                protocol_max_selected_control_bytes_ =
                    (std::max)(protocol_max_selected_control_bytes_, entry.control_encoded_bytes);
            }
            if constexpr (p_traits::global_controls_then_positions) {
                released_full_query_hash_bytes = bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(protocol_query_hashes_.capacity()),
                    sizeof(hash_type));
                std::vector<hash_type>{}.swap(protocol_query_hashes_);
            }
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_route_allocation,
                                       projected,
                                       0u,
                                       0u,
                                       route_bytes);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t query_record_capacity_bytes = protocol_query_state_capacity_bytes_();
        const std::uint64_t compact_query_hash_capacity_bytes =
            protocol_query_hash_capacity_bytes_();
        const std::uint64_t radix_offset_capacity_bytes = bioseq::common::saturating_mul(
            protocol_query_radix_offset_count_, sizeof(std::uint32_t));
        const std::uint64_t ordinal_map_capacity_bytes = bioseq::common::saturating_mul(
            protocol_ordinal_to_selected_.capacity(), sizeof(std::size_t));
        const std::uint64_t selected_extent_vector_capacity_bytes = bioseq::common::saturating_mul(
            protocol_selected_extents_.capacity(), sizeof(selected_extent));
        if (query_record_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            compact_query_hash_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            released_full_query_hash_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            radix_offset_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            ordinal_map_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            selected_extent_vector_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            !replace_ledger_field_(protocol_owned_bytes_.query_record_bytes,
                                   query_record_capacity_bytes) ||
            !replace_ledger_field_(protocol_owned_bytes_.query_hash_bytes,
                                   compact_query_hash_capacity_bytes) ||
            !replace_ledger_field_(protocol_owned_bytes_.radix_offset_bytes,
                                   radix_offset_capacity_bytes) ||
            !replace_ledger_field_(protocol_owned_bytes_.ordinal_map_bytes,
                                   ordinal_map_capacity_bytes) ||
            !replace_ledger_field_(protocol_owned_bytes_.selected_extent_vector_bytes,
                                   selected_extent_vector_capacity_bytes)) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        add_memory_stats_(protocol_memory_stats_.released_query_hash_capacity_bytes,
                          released_full_query_hash_bytes);
        if (inject_admission_failure_(grouped_soa_query_admission_stage::query_route_reconcile)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_route_reconcile,
                                       projected,
                                       0u,
                                       0u,
                                       route_bytes,
                                       nullptr,
                                       true);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        if (!route_guard.reconcile()) {
            capture_admission_failure_(grouped_soa_query_admission_stage::query_route_reconcile,
                                       projected,
                                       0u,
                                       0u,
                                       route_bytes);
            return protocol_poison<void>(minimizer_generation_encoding_error::admission);
        }
        if (!protocol_ledger_matches_oracle_()) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        initialize_physical_cursor();
        protocol_initialized_ = true;
        return {};
    }

    /// Reserve the largest selected control extent once. extent_session owns
    /// only cursor/checksum state and borrows this buffer. No per-extent
    /// reserve/deallocate cycle remains on the cold-load path.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    ensure_control_scratch_() {
        if (!protocol_control_scratch_.empty()) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (protocol_owned_bytes_.control_scratch_bytes != 0u)
            return {};
        if (protocol_max_selected_control_bytes_ > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t headroom = bioseq::common::saturating_mul(
            protocol_max_selected_control_bytes_,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t projected =
            bioseq::common::saturating_add(protocol_owned_capacity_bytes(), headroom);
        if (headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        permit_growth_guard guard{*this};
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::control_payload_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::control_payload_reservation,
                projected,
                0u,
                0u,
                headroom,
                nullptr,
                true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure payload_failure;
        if (!guard.grow(projected, &payload_failure)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::control_payload_reservation,
                projected,
                0u,
                0u,
                headroom,
                &payload_failure);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::control_payload_allocation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::control_payload_allocation,
                projected,
                0u,
                0u,
                headroom,
                nullptr,
                true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        try {
            protocol_control_scratch_.reserve(
                static_cast<std::size_t>(protocol_max_selected_control_bytes_));
        } catch (const std::bad_alloc&) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::control_payload_allocation,
                projected,
                0u,
                0u,
                headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t actual_bytes = protocol_control_scratch_.capacity();
        if (!replace_ledger_field_(protocol_owned_bytes_.control_scratch_bytes, actual_bytes)) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::control_payload_reconcile)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::control_payload_reconcile,
                                       projected,
                                       0u,
                                       0u,
                                       headroom,
                                       nullptr,
                                       true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!guard.reconcile()) {
            capture_admission_failure_(grouped_soa_query_admission_stage::control_payload_reconcile,
                                       projected,
                                       0u,
                                       0u,
                                       headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    observe_control_extent(const grouped_soa_bucket_directory_entry& entry,
                           std::span<const std::byte> bytes) {
        const std::size_t selected_index = entry.partition_id < protocol_ordinal_to_selected_.size()
                                               ? protocol_ordinal_to_selected_[entry.partition_id]
                                               : no_route;
        std::uint64_t transient = bioseq::common::saturating_mul(
            entry.control_element_count, sizeof(typename wire_ops::control_type));
        // Keep every boundary in an intersected extent for strict per-control
        // position-order validation. grouped-SoA stores query routes sparsely;
        // admit the conservative all-routes peak before the one decode pass.
        const std::uint64_t retained_control_count =
            selected_index == no_route ? 0u : entry.control_element_count;
        const std::uint64_t retained_control_width =
            p_traits::global_controls_then_positions
                ? static_cast<std::uint8_t>(
                      prepared_query_scalar_width_for_max(entry.position_element_count))
                : sizeof(selected_control);
        std::uint64_t retained =
            bioseq::common::saturating_mul(retained_control_count, retained_control_width);
        if constexpr (p_traits::global_controls_then_positions) {
            // One dense u32 route image is always built.  Sparse conversion
            // happens only below 50% density, so its peak is bounded by one
            // additional dense-image worth of bytes.
            const std::uint64_t dense_route_bytes =
                bioseq::common::saturating_mul(retained_control_count, sizeof(std::uint32_t));
            retained = bioseq::common::saturating_add(retained, dense_route_bytes);
            retained = bioseq::common::saturating_add(retained, dense_route_bytes);
        }
        const std::uint64_t additional = bioseq::common::saturating_add(transient, retained);
        const std::uint64_t transient_headroom = bioseq::common::saturating_mul(
            transient, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t retained_headroom = [&retained]() noexcept {
            if constexpr (p_traits::global_controls_then_positions) {
                // grouped-SoA ends and route images are exact arrays. Their
                // admitted peak is already represented by retained and must
                // not receive vector growth headroom a second time.
                return retained;
            } else {
                return bioseq::common::saturating_mul(
                    retained, bioseq::parallelism::k_vector_preallocation_headroom_factor);
            }
        }();
        const std::uint64_t headroom =
            bioseq::common::saturating_add(transient_headroom, retained_headroom);
        permit_growth_guard guard{*this};
        const std::uint64_t projected =
            bioseq::common::saturating_add(protocol_owned_capacity_bytes(), headroom);
        if (additional == (std::numeric_limits<std::uint64_t>::max)() ||
            headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            projected == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::control_decode_reservation)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::control_decode_reservation,
                projected,
                0u,
                0u,
                headroom,
                nullptr,
                true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        bioseq::parallelism::memory_admission_failure control_failure;
        if (!guard.grow(projected, &control_failure)) {
            capture_admission_failure_(
                grouped_soa_query_admission_stage::control_decode_reservation,
                projected,
                0u,
                0u,
                headroom,
                &control_failure);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::control_decode_allocation)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_allocation,
                                       projected,
                                       0u,
                                       0u,
                                       headroom,
                                       nullptr,
                                       true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        const auto decoded_result = [this, &bytes, &entry, &selected_index, &projected, &headroom]()
            -> std::expected<void, minimizer_generation_encoding_error> {
            try {
                const auto decoded = wire_ops::decode_controls(bytes, entry, this->profile_);
                if (!decoded)
                    return std::unexpected(decoded.error());
                add_memory_stats_(protocol_memory_stats_.decoded_control_records,
                                  static_cast<std::uint64_t>(decoded->size()));
                if (selected_index == no_route)
                    return {};
                selected_extent& selected = protocol_selected_extents_[selected_index];
                const std::size_t retained_before = selected_control_count_(selected);
                if (retained_before != 0u || selected.controls_complete) {
                    return std::unexpected(minimizer_generation_encoding_error::row_order);
                }
                if constexpr (p_traits::global_controls_then_positions) {
                    prepared_query_scalar_array position_ends;
                    std::unique_ptr<std::uint32_t[]> dense_routes;
                    if (decoded->size() != 0u) {
                        position_ends.allocate(
                            prepared_query_scalar_width_for_max(entry.position_element_count),
                            decoded->size(),
                            false);
                        dense_routes =
                            std::make_unique_for_overwrite<std::uint32_t[]>(decoded->size());
                        std::fill_n(dense_routes.get(), decoded->size(), no_narrow_route);
                    }
                    // Publish both exact owners together only after every
                    // allocation succeeds, keeping failure stats exact.
                    selected.control_state.position_ends = std::move(position_ends);
                    selected.route_state.dense_routes = std::move(dense_routes);
                    selected.control_state.control_count = decoded->size();
                } else {
                    selected.control_state.controls.reserve(decoded->size());
                }
                std::uint64_t expected_position_begin = 0u;
                std::size_t control_ordinal = 0u;
                std::size_t routed_control_count = 0u;
                std::uint64_t route_hash_probes = 0u;
                if constexpr (p_traits::global_controls_then_positions) {
                    constexpr std::uint64_t max_probes_per_lookup =
                        std::numeric_limits<std::uint32_t>::digits + 1u;
                    if (decoded->size() >
                        (std::numeric_limits<std::uint64_t>::max)() / max_probes_per_lookup) {
                        return std::unexpected(
                            minimizer_generation_encoding_error::arithmetic_overflow);
                    }
                }
                for (const auto& control : *decoded) {
                    std::uint32_t control_hash_probes = 0u;
                    const std::size_t query_index =
                        query_index_for_control_hash(wire_ops::hash(control), control_hash_probes);
                    route_hash_probes += control_hash_probes;
                    const std::uint64_t begin = wire_ops::position_begin(control);
                    const std::uint64_t end = wire_ops::position_end(control);
                    // Both wire codecs already enforce this partition while
                    // decoding.  Recheck it at the retained-image boundary so
                    // position_begin does not need to survive per control.
                    if (begin != expected_position_begin || end <= begin) {
                        return std::unexpected(minimizer_generation_encoding_error::control_range);
                    }
                    expected_position_begin = end;
                    if (query_index != no_route) {
                        const std::uint64_t count = end - begin;
                        if constexpr (p_traits::global_controls_then_positions) {
                            if (auto added = add_protocol_query_exact_count_(query_index, count);
                                !added) {
                                return added;
                            }
                        } else {
                            query_record& record = protocol_query_records_[query_index];
                            if (record.global_occurrence_count >
                                (std::numeric_limits<std::uint64_t>::max)() - count) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::arithmetic_overflow);
                            }
                            record.global_occurrence_count += count;
                            if (record.global_occurrence_count > protocol_max_occ_) {
                                const std::uint64_t old_position_bytes =
                                    bioseq::common::saturating_mul(record.positions.capacity(),
                                                                   sizeof(std::uint64_t));
                                std::vector<std::uint64_t>{}.swap(record.positions);
                                if (old_position_bytes ==
                                        (std::numeric_limits<std::uint64_t>::max)() ||
                                    old_position_bytes >
                                        protocol_owned_bytes_.query_record_position_bytes ||
                                    !replace_ledger_field_(
                                        protocol_owned_bytes_.query_record_position_bytes,
                                        protocol_owned_bytes_.query_record_position_bytes -
                                            old_position_bytes)) {
                                    return std::unexpected(
                                        minimizer_generation_encoding_error::arithmetic_overflow);
                                }
                                record.positions_complete = false;
                            }
                        }
                    }
                    if constexpr (p_traits::global_controls_then_positions) {
                        if (!selected.control_state.position_ends.set(control_ordinal, end)) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::control_range);
                        }
                        if (query_index != no_route) {
                            if (query_index >= no_narrow_route) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::arithmetic_overflow);
                            }
                            selected.route_state.dense_routes[control_ordinal] =
                                static_cast<std::uint32_t>(query_index);
                            ++routed_control_count;
                        }
                    } else {
                        selected.control_state.controls.push_back(selected_control{
                            .query_index = query_index,
                            .position_end = end,
                        });
                    }
                    ++control_ordinal;
                }
                if (expected_position_begin != entry.position_element_count) {
                    return std::unexpected(minimizer_generation_encoding_error::control_range);
                }
                if constexpr (p_traits::global_controls_then_positions) {
                    const std::size_t control_count = selected.control_state.control_count;
                    if (control_count <= no_narrow_route &&
                        routed_control_count < control_count - routed_control_count) {
                        if (routed_control_count != 0u) {
                            selected.route_state.sparse_routes =
                                std::make_unique_for_overwrite<sparse_selected_route[]>(
                                    routed_control_count);
                        }
                        std::size_t sparse_index = 0u;
                        for (std::size_t ordinal = 0u; ordinal < control_count; ++ordinal) {
                            const std::uint32_t query_route =
                                selected.route_state.dense_routes[ordinal];
                            if (query_route == no_narrow_route)
                                continue;
                            selected.route_state.sparse_routes[sparse_index++] =
                                sparse_selected_route{
                                    .control_ordinal = static_cast<std::uint32_t>(ordinal),
                                    .query_index = query_route,
                                };
                        }
                        if (sparse_index != routed_control_count) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::element_count);
                        }
                        selected.route_state.sparse_route_count = routed_control_count;
                        selected.route_state.dense_routes.reset();
                        selected.route_state.uses_sparse = true;
                    }
                    // Update stats once per decoded extent.  A counter write
                    // on every binary-search comparison would itself become
                    // material at hundreds of millions of controls.
                    add_memory_stats_(protocol_memory_stats_.query_route_control_lookups,
                                      static_cast<std::uint64_t>(decoded->size()));
                    add_memory_stats_(protocol_memory_stats_.query_route_hash_probes,
                                      route_hash_probes);
                    add_memory_stats_(protocol_memory_stats_.query_route_matches,
                                      static_cast<std::uint64_t>(routed_control_count));
                }
                add_memory_stats_(protocol_memory_stats_.retained_selected_control_records,
                                  static_cast<std::uint64_t>(selected_control_count_(selected) -
                                                             retained_before));
                add_memory_stats_(protocol_memory_stats_.retained_selected_control_capacity_bytes,
                                  selected_extent_control_capacity_bytes_(selected));
                if (!replace_selected_extent_owner_(
                        selected, selected_extent_control_capacity_bytes_(selected))) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                selected.controls_complete = true;
                return {};
            } catch (const std::bad_alloc&) {
                capture_admission_failure_(
                    grouped_soa_query_admission_stage::control_decode_allocation,
                    projected,
                    0u,
                    0u,
                    headroom);
                return std::unexpected(minimizer_generation_encoding_error::admission);
            }
        }();
        if (!decoded_result) {
            return std::unexpected(decoded_result.error());
        }
        if (inject_admission_failure_(
                grouped_soa_query_admission_stage::control_decode_reconcile)) {
            capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_reconcile,
                                       projected,
                                       0u,
                                       0u,
                                       headroom,
                                       nullptr,
                                       true);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        if (!guard.reconcile()) {
            capture_admission_failure_(grouped_soa_query_admission_stage::control_decode_reconcile,
                                       projected,
                                       0u,
                                       0u,
                                       headroom);
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    append_retained_position(std::size_t query_index, std::uint64_t value) {
        if constexpr (p_traits::global_controls_then_positions) {
            if (query_index >= protocol_query_count_) {
                return std::unexpected(minimizer_generation_encoding_error::element_count);
            }
            const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
            if (exact_count == 0u || exact_count > protocol_max_occ_) {
                return {};
            }
            const std::uint64_t position_offset =
                protocol_query_retained_(query_index)
                    ? protocol_query_position_offsets_[query_index]
                    : (std::numeric_limits<std::uint64_t>::max)();
            const std::uint64_t positions_written = protocol_query_written_.get(query_index);
            if (!protocol_global_position_storage_prepared_ ||
                !protocol_query_retained_(query_index) || positions_written >= exact_count ||
                position_offset > protocol_global_position_count_ ||
                positions_written > protocol_global_position_count_ - position_offset) {
                return std::unexpected(minimizer_generation_encoding_error::element_count);
            }
            const std::uint64_t destination = position_offset + positions_written;
            if (destination >= protocol_global_position_count_) {
                return std::unexpected(minimizer_generation_encoding_error::element_count);
            }
            if (positions_written != 0u &&
                value <= protocol_global_positions_[static_cast<std::size_t>(destination - 1u)]) {
                return std::unexpected(minimizer_generation_encoding_error::position_range);
            }
            protocol_global_positions_[static_cast<std::size_t>(destination)] = value;
            if (!protocol_query_written_.increment(query_index)) {
                return std::unexpected(minimizer_generation_encoding_error::element_count);
            }
            return {};
        } else {
            if (query_index >= protocol_query_records_.size()) {
                return std::unexpected(minimizer_generation_encoding_error::element_count);
            }
            query_record& record = protocol_query_records_[query_index];
            if (!record.positions_complete)
                return {};
            if (!record.positions.empty() && value <= record.positions.back()) {
                return std::unexpected(minimizer_generation_encoding_error::position_range);
            }
            if (record.positions.size() == record.positions.capacity()) {
                increment_memory_stats_(protocol_memory_stats_.position_append_growth_reservations);
                const std::size_t maximum = static_cast<std::size_t>(
                    std::min<std::uint64_t>(protocol_max_occ_, record.positions.max_size()));
                const std::size_t required = record.positions.size() + 1u;
                if (required > maximum) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                std::size_t wanted = record.positions.empty() ? std::min<std::size_t>(maximum, 8u)
                                     : record.positions.capacity() > maximum / 2u
                                         ? maximum
                                         : record.positions.capacity() * 2u;
                wanted = std::max(wanted, required);
                permit_growth_guard guard{*this};
                const std::uint64_t old_position_bytes = bioseq::common::saturating_mul(
                    record.positions.capacity(), sizeof(std::uint64_t));
                const std::uint64_t additional =
                    bioseq::common::saturating_mul(wanted, sizeof(std::uint64_t));
                const std::uint64_t projected = bioseq::common::saturating_add(
                    protocol_owned_capacity_bytes(),
                    bioseq::common::saturating_mul(
                        additional, bioseq::parallelism::k_vector_preallocation_headroom_factor));
                if (!guard.grow(projected)) {
                    return std::unexpected(minimizer_generation_encoding_error::admission);
                }
                try {
                    record.positions.reserve(wanted);
                } catch (const std::bad_alloc&) {
                    return std::unexpected(minimizer_generation_encoding_error::admission);
                }
                const std::uint64_t new_position_bytes = bioseq::common::saturating_mul(
                    record.positions.capacity(), sizeof(std::uint64_t));
                if (old_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                    new_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                    old_position_bytes > protocol_owned_bytes_.query_record_position_bytes) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                const std::uint64_t position_owner_without_old =
                    protocol_owned_bytes_.query_record_position_bytes - old_position_bytes;
                if (new_position_bytes >
                        (std::numeric_limits<std::uint64_t>::max)() - position_owner_without_old ||
                    !replace_ledger_field_(protocol_owned_bytes_.query_record_position_bytes,
                                           position_owner_without_old + new_position_bytes)) {
                    return std::unexpected(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                if (!guard.reconcile()) {
                    return std::unexpected(minimizer_generation_encoding_error::admission);
                }
            }
            record.positions.push_back(value);
            return {};
        }
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    observe_position_payload(const grouped_soa_bucket_directory_entry& entry,
                             std::span<const std::byte> payload) {
        const std::size_t width = static_cast<std::size_t>(this->profile_.position_width);
        if (width == 0u || payload.size() % width != 0u) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        const std::size_t selected_index = entry.partition_id < protocol_ordinal_to_selected_.size()
                                               ? protocol_ordinal_to_selected_[entry.partition_id]
                                               : no_route;
        selected_extent* selected =
            selected_index == no_route ? nullptr : &protocol_selected_extents_[selected_index];
        const auto core = derive_minimizer_position_segment_bounds(
            this->manifest_, static_cast<std::uint32_t>(entry.position_segment));
        if (!core)
            return std::unexpected(core.error());
        for (std::size_t offset = 0; offset < payload.size(); offset += width) {
            const auto decoded =
                wire_ops::decode_position(payload.subspan(offset, width), *core, this->profile_);
            if (!decoded)
                return std::unexpected(decoded.error());
            if (selected != nullptr) {
                while (selected->next_control < selected_control_count_(*selected) &&
                       selected->next_position >=
                           selected_position_end_(*selected, selected->next_control)) {
                    if constexpr (p_traits::global_controls_then_positions) {
                        auto& routes = selected->route_state;
                        if (routes.uses_sparse &&
                            routes.next_sparse_route < routes.sparse_route_count) {
                            const std::uint32_t route_ordinal =
                                routes.sparse_routes[routes.next_sparse_route].control_ordinal;
                            if (route_ordinal < selected->next_control) {
                                return std::unexpected(
                                    minimizer_generation_encoding_error::element_count);
                            }
                            if (route_ordinal == selected->next_control) {
                                ++routes.next_sparse_route;
                            }
                        }
                    }
                    ++selected->next_control;
                    selected->previous_position_valid = false;
                }
                if (selected->next_control < selected_control_count_(*selected)) {
                    std::size_t query_index = no_route;
                    if constexpr (p_traits::global_controls_then_positions) {
                        const auto& routes = selected->route_state;
                        if (routes.uses_sparse) {
                            if (routes.next_sparse_route < routes.sparse_route_count) {
                                const sparse_selected_route& route =
                                    routes.sparse_routes[routes.next_sparse_route];
                                if (route.control_ordinal < selected->next_control) {
                                    return std::unexpected(
                                        minimizer_generation_encoding_error::element_count);
                                }
                                if (route.control_ordinal == selected->next_control) {
                                    query_index = route.query_index;
                                }
                            }
                        } else if (!routes.dense_routes) {
                            return std::unexpected(
                                minimizer_generation_encoding_error::element_count);
                        } else {
                            const std::uint32_t route = routes.dense_routes[selected->next_control];
                            if (route != no_narrow_route)
                                query_index = route;
                        }
                    } else {
                        query_index =
                            selected->control_state.controls[selected->next_control].query_index;
                    }
                    if (selected->previous_position_valid &&
                        *decoded <= selected->previous_position) {
                        return std::unexpected(minimizer_generation_encoding_error::position_range);
                    }
                    selected->previous_position = *decoded;
                    selected->previous_position_valid = true;
                    if (query_index != no_route) {
                        if (auto appended = append_retained_position(query_index, *decoded);
                            !appended) {
                            return appended;
                        }
                    }
                }
                ++selected->next_position;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row_impl(const minimizer_generation_row_view& row,
                             bool row_checksum_prevalidated) {
        const auto entries = this->directory_.entries();
        if (protocol_failed_ || protocol_ready_ || protocol_entry_index_ >= entries.size()) {
            return protocol_poison<void>(minimizer_generation_encoding_error::row_order);
        }
        const grouped_soa_bucket_directory_entry& entry = entries[protocol_entry_index_];
        const bool positions = protocol_position_stage_;
        const std::size_t selected_index = entry.partition_id < protocol_ordinal_to_selected_.size()
                                               ? protocol_ordinal_to_selected_[entry.partition_id]
                                               : no_route;
        const bool selected = selected_index != no_route;
        const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
        if (!position_sub_key) {
            return protocol_poison<void>(position_sub_key.error());
        }
        const std::uint32_t chunk_count =
            positions ? entry.position_chunk_count : entry.control_chunk_count;
        const std::uint64_t encoded_bytes =
            positions ? entry.position_encoded_bytes : entry.control_encoded_bytes;
        if (!protocol_session_) {
            protocol_session_.emplace(extent_session{
                .entry_index = protocol_entry_index_,
                .positions = positions,
            });
            if (!positions && selected) {
                if (encoded_bytes > (std::numeric_limits<std::size_t>::max)()) {
                    protocol_session_.reset();
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                if (auto scratch = ensure_control_scratch_(); !scratch) {
                    protocol_session_.reset();
                    return protocol_poison<void>(scratch.error());
                }
                if (!protocol_control_scratch_.empty() ||
                    protocol_control_scratch_.capacity() <
                        static_cast<std::size_t>(encoded_bytes)) {
                    protocol_session_.reset();
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::element_count);
                }
            }
        }
        extent_session& session = *protocol_session_;
        if (session.entry_index != protocol_entry_index_ || session.positions != positions ||
            session.next_chunk >= chunk_count) {
            return protocol_poison<void>(minimizer_generation_encoding_error::row_order);
        }
        constexpr std::uint32_t extent_payload_bytes =
            minimizer_extent_payload_bytes(p_traits::layout);
        static_assert(extent_payload_bytes != 0);
        const std::uint64_t byte_offset =
            static_cast<std::uint64_t>(session.next_chunk) * extent_payload_bytes;
        if (byte_offset >= encoded_bytes) {
            return protocol_poison<void>(minimizer_generation_encoding_error::row_order);
        }
        const std::uint64_t expected_size =
            std::min<std::uint64_t>(extent_payload_bytes, encoded_bytes - byte_offset);
        const std::uint32_t first_chunk =
            positions ? entry.position_first_chunk : entry.control_first_chunk;
        const index_sub_key_t expected_sub_key = positions ? *position_sub_key : entry.sub_key;
        if (row.address !=
                index_chunk_address{
                    .generation_id = this->head_.generation_id,
                    .position_segment = entry.position_segment,
                    .sub_key = expected_sub_key,
                    .chunk_no = first_chunk + session.next_chunk,
                } ||
            row.bytes.size() != expected_size) {
            return protocol_poison<void>(minimizer_generation_encoding_error::row_order);
        }
        if (!row_checksum_prevalidated &&
            row.row_checksum != crc64_index_chunk_row(row.address.generation_id,
                                                      row.address.position_segment,
                                                      row.address.sub_key,
                                                      row.address.chunk_no,
                                                      static_cast<std::uint32_t>(row.bytes.size()),
                                                      row.bytes)) {
            return protocol_poison<void>(minimizer_generation_encoding_error::checksum);
        }
        // Raw callers reached this point only after their mandatory row CRC
        // traversal above; receipt callers carry the same authenticated
        // address-bound checksum. Compose the extent CRC algebraically in
        // both cases and never reread the payload solely for another CRC.
        session.crc.value =
            crc64_append_validated_index_chunk_payload(session.crc.checksum(),
                                                       row.address,
                                                       static_cast<std::uint32_t>(row.bytes.size()),
                                                       row.row_checksum);
        if (positions && selected) {
            if (auto observed = observe_position_payload(entry, row.bytes); !observed) {
                return protocol_poison<void>(observed.error());
            }
        } else if (!positions && selected) {
            if (protocol_control_scratch_.size() > static_cast<std::size_t>(encoded_bytes) ||
                row.bytes.size() >
                    protocol_control_scratch_.capacity() - protocol_control_scratch_.size() ||
                row.bytes.size() >
                    static_cast<std::size_t>(encoded_bytes) - protocol_control_scratch_.size()) {
                return protocol_poison<void>(minimizer_generation_encoding_error::element_count);
            }
            protocol_control_scratch_.insert(
                protocol_control_scratch_.end(), row.bytes.begin(), row.bytes.end());
        }
        ++session.next_chunk;
        if (session.next_chunk != chunk_count)
            return {};
        const std::uint64_t expected_crc =
            positions ? entry.position_checksum : entry.control_checksum;
        if (session.crc.checksum() != expected_crc) {
            return protocol_poison<void>(minimizer_generation_encoding_error::checksum);
        }
        if (!positions && selected) {
            const auto observed = observe_control_extent(entry, protocol_control_scratch_);
            if (!observed) {
                return protocol_poison<void>(observed.error());
            }
            protocol_control_scratch_.clear();
        } else if (positions && selected) {
            selected_extent& extent = protocol_selected_extents_[selected_index];
            while (extent.next_control < selected_control_count_(extent) &&
                   extent.next_position >= selected_position_end_(extent, extent.next_control)) {
                if constexpr (p_traits::global_controls_then_positions) {
                    auto& routes = extent.route_state;
                    if (routes.uses_sparse &&
                        routes.next_sparse_route < routes.sparse_route_count) {
                        const std::uint32_t route_ordinal =
                            routes.sparse_routes[routes.next_sparse_route].control_ordinal;
                        if (route_ordinal < extent.next_control) {
                            return protocol_poison<void>(
                                minimizer_generation_encoding_error::element_count);
                        }
                        if (route_ordinal == extent.next_control) {
                            ++routes.next_sparse_route;
                        }
                    }
                }
                ++extent.next_control;
                extent.previous_position_valid = false;
            }
            bool routes_complete = true;
            if constexpr (p_traits::global_controls_then_positions) {
                const auto& routes = extent.route_state;
                routes_complete =
                    routes.uses_sparse
                        ? !routes.dense_routes &&
                              routes.next_sparse_route == routes.sparse_route_count
                        : !routes.sparse_routes && (selected_control_count_(extent) == 0u ||
                                                    routes.dense_routes != nullptr);
            }
            if (!extent.controls_complete || extent.next_position != entry.position_element_count ||
                extent.next_control != selected_control_count_(extent) || !routes_complete) {
                return protocol_poison<void>(minimizer_generation_encoding_error::element_count);
            }
            extent.positions_complete = true;
            if constexpr (p_traits::global_controls_then_positions) {
                extent.control_state.position_ends.reset();
                extent.control_state.control_count = 0u;
                extent.route_state.dense_routes.reset();
                extent.route_state.sparse_routes.reset();
                extent.route_state.sparse_route_count = 0u;
                extent.route_state.next_sparse_route = 0u;
                extent.route_state.uses_sparse = false;
            } else {
                std::vector<selected_control>{}.swap(extent.control_state.controls);
            }
            if (!replace_selected_extent_owner_(extent, 0u)) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        protocol_session_.reset();
        advance_physical_cursor();
        if constexpr (p_traits::global_controls_then_positions) {
            if (protocol_position_stage_ && !protocol_global_position_storage_prepared_) {
                if (!release_control_scratch_noexcept_()) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                this->permit_.shrink_to(protocol_owned_capacity_bytes());
                if (!protocol_ledger_matches_oracle_()) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::arithmetic_overflow);
                }
                if (auto prepared = prepare_global_position_storage(); !prepared) {
                    return protocol_poison<void>(prepared.error());
                }
            }
        }
        this->permit_.shrink_to(protocol_owned_capacity_bytes());
        return {};
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row(const minimizer_generation_row_view& row) {
        return accept_protocol_row_impl(row, false);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row(const validated_minimizer_generation_row_receipt& receipt) {
        if (!receipt.has_valid_authority()) {
            return protocol_poison<void>(minimizer_generation_encoding_error::checksum);
        }
        return accept_protocol_row_impl(
            minimizer_generation_row_view{
                .address = receipt.address(),
                .bytes = receipt.bytes(),
                .row_checksum = receipt.row_checksum(),
            },
            true);
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish_query_protocol() {
        if (protocol_failed_ || protocol_ready_ || protocol_session_ ||
            protocol_entry_index_ != this->directory_.entries().size() ||
            protocol_position_stage_) {
            return protocol_poison<void>(minimizer_generation_encoding_error::row_count);
        }
        for (const selected_extent& extent : protocol_selected_extents_) {
            if (!extent.controls_complete || !extent.positions_complete) {
                return protocol_poison<void>(minimizer_generation_encoding_error::row_count);
            }
        }
        if constexpr (p_traits::global_controls_then_positions) {
            for (std::size_t query_index = 0u; query_index < protocol_query_count_; ++query_index) {
                const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
                const bool retained = protocol_query_retained_(query_index);
                const std::uint64_t written = protocol_query_written_.get(query_index);
                if (exact_count == 0u || exact_count > protocol_max_occ_) {
                    if (retained || written != 0u) {
                        return protocol_poison<void>(
                            minimizer_generation_encoding_error::element_count);
                    }
                    continue;
                }
                const std::uint64_t position_offset = protocol_query_position_offsets_[query_index];
                if (!retained || written != exact_count ||
                    position_offset > protocol_global_position_count_ ||
                    exact_count > protocol_global_position_count_ - position_offset) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::element_count);
                }
            }
            protocol_query_written_.reset();
            if (!replace_ledger_field_(protocol_owned_bytes_.query_record_bytes,
                                       protocol_query_state_capacity_bytes_())) {
                return protocol_poison<void>(
                    minimizer_generation_encoding_error::arithmetic_overflow);
            }
        } else {
            for (const query_record& record : protocol_query_records_) {
                if (record.positions_complete &&
                    record.positions.size() != record.global_occurrence_count) {
                    return protocol_poison<void>(
                        minimizer_generation_encoding_error::element_count);
                }
            }
        }
        if (!release_control_scratch_noexcept_()) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        this->permit_.shrink_to(protocol_owned_capacity_bytes());
        if (!protocol_ledger_matches_oracle_()) {
            return protocol_poison<void>(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        protocol_ready_ = true;
        return {};
    }

    struct protocol_lookup {
        std::uint64_t global_occurrence_count = 0;
        bool positions_complete = true;
        std::span<const std::uint64_t> positions{};
    };

    [[nodiscard]] protocol_lookup lookup_protocol(std::uint64_t hash) const noexcept {
        if (!protocol_ready_ || hash > (std::numeric_limits<hash_type>::max)())
            return {};
        const std::size_t query_index = query_index_for_hash(hash);
        if (query_index == no_route || query_index >= protocol_query_state_count_())
            return {};
        std::span<const std::uint64_t> positions;
        if constexpr (p_traits::global_controls_then_positions) {
            const std::uint64_t exact_count = protocol_query_exact_count_(query_index);
            const bool retained = protocol_query_retained_(query_index);
            const bool positions_complete = exact_count == 0u || retained;
            if (retained && exact_count != 0u &&
                protocol_query_position_offsets_[query_index] <= protocol_global_position_count_ &&
                exact_count <= protocol_global_position_count_ -
                                   protocol_query_position_offsets_[query_index]) {
                positions = std::span<const std::uint64_t>{
                    protocol_global_positions_.get() +
                        static_cast<std::size_t>(protocol_query_position_offsets_[query_index]),
                    static_cast<std::size_t>(exact_count)};
            }
            return protocol_lookup{
                .global_occurrence_count = exact_count,
                .positions_complete = positions_complete,
                .positions = positions,
            };
        } else {
            const query_record& found = protocol_query_records_[query_index];
            positions = found.positions;
            return protocol_lookup{
                .global_occurrence_count = found.global_occurrence_count,
                .positions_complete = found.positions_complete,
                .positions = positions,
            };
        }
    }

    [[nodiscard]] std::size_t protocol_retained_position_count() const noexcept {
        if constexpr (p_traits::global_controls_then_positions) {
            return protocol_global_position_count_;
        } else {
            std::size_t count = 0;
            for (const query_record& record : protocol_query_records_) {
                if (record.positions.size() > (std::numeric_limits<std::size_t>::max)() - count) {
                    return (std::numeric_limits<std::size_t>::max)();
                }
                count += record.positions.size();
            }
            return count;
        }
    }

    [[nodiscard]] const grouped_soa_query_admission_stats&
    protocol_admission_failure_stats() const noexcept {
        return protocol_admission_failure_stats_;
    }
};

} // namespace minimizer_prepared_reader_detail

/// Lookup result containing the global count and any retained positions.
struct grouped_soa_query_lookup {
    std::uint64_t global_occurrence_count = 0;
    bool positions_complete = true;
    std::span<const std::uint64_t> positions{};
};

/// Reader-owned stats for the grouped-SoA cold-load memory path.  In
/// particular, `position_append_growth_reservations == 0` proves that payload
/// ingestion did not re-run live memory admission (and its OS probes) while
/// appending individual retained positions.
struct grouped_soa_query_reader_memory_stats {
    std::uint64_t capacity_accounting_passes = 0;
    std::uint64_t global_position_storage_preparations = 0;
    std::uint64_t global_position_storage_allocations = 0;
    std::uint64_t position_append_growth_reservations = 0;
    std::uint64_t retained_position_count = 0;
    // Exact allocated array elements. For grouped-SoA this must equal the
    // retained count; the exported capacity name remains stable for E2E SQL.
    std::uint64_t retained_position_capacity = 0;
    std::uint64_t query_record_size_bytes = 0;
    std::uint64_t query_record_count = 0;
    std::uint64_t query_record_capacity = 0;
    std::uint64_t query_record_capacity_bytes = 0;
    std::uint64_t query_record_position_capacity_bytes = 0;
    // Exact bucket-implied query suffixes plus high-bit radix offsets. The
    // aggregate SQL key retains this stable name and exact owner equation.
    std::uint64_t query_hash_capacity_bytes = 0;
    std::uint64_t released_query_hash_capacity_bytes = 0;
    std::uint64_t query_route_radix_bits = 0;
    std::uint64_t query_route_radix_bucket_count = 0;
    std::uint64_t query_route_radix_nonempty_bucket_count = 0;
    std::uint64_t query_route_radix_max_bucket_size = 0;
    std::uint64_t query_route_control_lookups = 0;
    std::uint64_t query_route_hash_probes = 0;
    std::uint64_t query_route_matches = 0;
    std::uint64_t decoded_control_records = 0;
    std::uint64_t retained_selected_control_records = 0;
    std::uint64_t selected_control_size_bytes = 0;
    std::uint64_t retained_selected_control_capacity_bytes = 0;
    std::uint64_t current_selected_control_capacity_bytes = 0;
    std::uint64_t directory_capacity_bytes = 0;
    std::uint64_t ordinal_route_capacity_bytes = 0;
    std::uint64_t selected_extent_capacity_bytes = 0;
    // Exact array bytes, not allocator headroom or vector capacity.
    std::uint64_t global_position_capacity_bytes = 0;
    std::uint64_t control_payload_capacity_bytes = 0;
    std::uint64_t owned_capacity_bytes = 0;
    bool saturated = false;

    [[nodiscard]] bool retained_layout_equation_holds() const noexcept {
        using bioseq::common::saturating_add;
        std::uint64_t accounted = directory_capacity_bytes;
        accounted = saturating_add(accounted, query_hash_capacity_bytes);
        accounted = saturating_add(accounted, query_record_capacity_bytes);
        accounted = saturating_add(accounted, query_record_position_capacity_bytes);
        accounted = saturating_add(accounted, ordinal_route_capacity_bytes);
        accounted = saturating_add(accounted, selected_extent_capacity_bytes);
        accounted = saturating_add(accounted, current_selected_control_capacity_bytes);
        accounted = saturating_add(accounted, global_position_capacity_bytes);
        accounted = saturating_add(accounted, control_payload_capacity_bytes);
        return accounted != (std::numeric_limits<std::uint64_t>::max)() &&
               accounted == owned_capacity_bytes;
    }
};

/// Backend-independent scalar grouped-SoA reader. The facade consumes the same
/// grouped_soa_directory_view/address authority as the prepared readers
/// and accepts only the grouped-SoA layout.
class prepared_grouped_soa_query_reader
    : private minimizer_prepared_reader_detail::trait_driven_prepared_query_core<
          minimizer_prepared_reader_detail::codec_traits<
              minimizer_generation_layout::grouped_soa_segment_local>> {
  private:
    using protocol_core = minimizer_prepared_reader_detail::trait_driven_prepared_query_core<
        minimizer_prepared_reader_detail::codec_traits<
            minimizer_generation_layout::grouped_soa_segment_local>>;

    prepared_grouped_soa_query_reader() = default;

  public:
    /// Transfer an admitted reader while invalidating the source protocol state.
    prepared_grouped_soa_query_reader(prepared_grouped_soa_query_reader&& other) noexcept
        : protocol_core(std::move(static_cast<protocol_core&>(other))) {
        other.mark_protocol_moved_from();
    }
    prepared_grouped_soa_query_reader& operator=(prepared_grouped_soa_query_reader&&) = delete;
    prepared_grouped_soa_query_reader(const prepared_grouped_soa_query_reader&) = delete;
    prepared_grouped_soa_query_reader& operator=(const prepared_grouped_soa_query_reader&) = delete;

    /// Begin a reader for a sorted unique query-hash set.
    [[nodiscard]] static std::expected<prepared_grouped_soa_query_reader,
                                       minimizer_generation_encoding_error>
    begin(bioseq::parallelism::memory_admission_controller& admission,
          const minimizer_generation_head& head,
          const minimizer_generation_manifest& manifest,
          const expected_minimizer_generation_identity& expected,
          std::span<const std::byte> encoded_directory,
          std::span<const std::uint64_t> query_hashes,
          std::uint64_t max_occ,
          grouped_soa_query_admission_stats* admission_failure_stats = nullptr,
          grouped_soa_query_reader_options test_options = {}) {
        if (admission_failure_stats != nullptr) {
            *admission_failure_stats = {};
        }
        if (manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
            expected.layout != minimizer_generation_layout::grouped_soa_segment_local) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || !expected.max_occ_cap || *expected.max_occ_cap != profile->max_occ_cap ||
            profile->max_occ_cap == (std::numeric_limits<std::uint64_t>::max)() ||
            max_occ > profile->max_occ_cap) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        prepared_grouped_soa_query_reader reader;
        if (auto initialized = reader.initialize_query_protocol(admission,
                                                                head,
                                                                manifest,
                                                                expected,
                                                                *profile,
                                                                encoded_directory,
                                                                query_hashes,
                                                                max_occ,
                                                                test_options);
            !initialized) {
            if (admission_failure_stats != nullptr) {
                *admission_failure_stats = reader.protocol_admission_failure_stats();
            }
            return std::unexpected(initialized.error());
        }
        return reader;
    }

    /// Direct canonical-query overload used by the scheduler. The input must
    /// already be strictly sorted and unique by `kmer()`. It is validated while
    /// copying once into the reader-owned compact hash image, avoiding the
    /// scheduler's temporary u64 vector, permit, and a second sort/unique.
    [[nodiscard]] static std::expected<prepared_grouped_soa_query_reader,
                                       minimizer_generation_encoding_error>
    begin(bioseq::parallelism::memory_admission_controller& admission,
          const minimizer_generation_head& head,
          const minimizer_generation_manifest& manifest,
          const expected_minimizer_generation_identity& expected,
          std::span<const std::byte> encoded_directory,
          std::span<const minimap_sketch_data> sorted_unique_query_minimizers,
          std::uint64_t max_occ,
          grouped_soa_query_admission_stats* admission_failure_stats = nullptr,
          grouped_soa_query_reader_options test_options = {}) {
        if (admission_failure_stats != nullptr) {
            *admission_failure_stats = {};
        }
        if (manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
            expected.layout != minimizer_generation_layout::grouped_soa_segment_local) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || !expected.max_occ_cap || *expected.max_occ_cap != profile->max_occ_cap ||
            max_occ > profile->max_occ_cap) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        prepared_grouped_soa_query_reader reader;
        if (auto initialized = reader.initialize_query_protocol(admission,
                                                                head,
                                                                manifest,
                                                                expected,
                                                                *profile,
                                                                encoded_directory,
                                                                sorted_unique_query_minimizers,
                                                                max_occ,
                                                                test_options,
                                                                true);
            !initialized) {
            if (admission_failure_stats != nullptr) {
                *admission_failure_stats = reader.protocol_admission_failure_stats();
            }
            return std::unexpected(initialized.error());
        }
        return reader;
    }

    /// Authenticate and consume one callback-scoped generation row.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const minimizer_generation_row_view& row) {
        return this->accept_protocol_row(row);
    }

    /// Consume one authenticated generation-row receipt.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept(const validated_minimizer_generation_row_receipt& receipt) {
        return this->accept_protocol_row(receipt);
    }

    /// Finish ordered ingestion and make successful lookups available.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error> finish() {
        return this->finish_query_protocol();
    }

    /// Return the prepared result for one query hash.
    [[nodiscard]] grouped_soa_query_lookup lookup(std::uint64_t hash) const noexcept {
        const auto found = this->lookup_protocol(hash);
        return grouped_soa_query_lookup{
            .global_occurrence_count = found.global_occurrence_count,
            .positions_complete = found.positions_complete,
            .positions = found.positions,
        };
    }

    /// Return the captured failure snapshot for an unsuccessful begin operation.
    [[nodiscard]] const grouped_soa_query_admission_stats&
    admission_failure_stats() const noexcept {
        return this->protocol_admission_failure_stats();
    }

    /// Direct seed-consumer surface. A hash that
    /// was not part of the immutable query union is a caller error rather than
    /// an authenticated absence.  This keeps minimap_port on the native
    /// persisted representation and prevents representation conversion.
    [[nodiscard]] minimizer_lookup_spans lookup_spans(std::uint64_t hash) const {
        if (!ready()) {
            throw persisted_minimizer_lookup_error("grouped-SoA persisted lookup is not ready");
        }
        if (hash > this->profile_.max_hash ||
            this->query_index_for_hash(hash) == (std::numeric_limits<std::size_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "grouped-SoA persisted lookup hash was not prepared");
        }
        const auto found = this->lookup_protocol(hash);
        if (found.global_occurrence_count == 0) {
            return minimizer_lookup_spans::contiguous(std::span<const std::uint64_t>{});
        }
        if (!found.positions_complete) {
            return minimizer_lookup_spans::positions_unavailable(found.global_occurrence_count);
        }
        if (found.positions.size() != found.global_occurrence_count) {
            throw persisted_minimizer_lookup_error(
                "grouped-SoA persisted lookup retained span is inconsistent");
        }
        return minimizer_lookup_spans::contiguous(found.positions);
    }

    /// Return whether row ingestion has failed closed.
    [[nodiscard]] bool failed() const noexcept {
        return this->protocol_failed_;
    }

    /// Return whether ordered ingestion completed successfully.
    [[nodiscard]] bool ready() const noexcept {
        return this->protocol_ready_ && !this->protocol_failed_;
    }

    /// Return the authenticated target length.
    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return this->manifest_.target_length;
    }

    /// Return bytes retained by the active memory permit.
    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return this->permit_.bytes();
    }

    /// Return exact ownership and ingestion counters for this reader.
    [[nodiscard]] grouped_soa_query_reader_memory_stats memory_stats() const noexcept {
        using bioseq::common::saturating_add;
        const auto& ledger = this->protocol_owned_bytes_;
        const std::uint64_t query_hash_capacity_bytes =
            saturating_add(ledger.query_hash_bytes, ledger.radix_offset_bytes);
        return grouped_soa_query_reader_memory_stats{
            .capacity_accounting_passes = this->protocol_memory_stats_.capacity_accounting_passes,
            .global_position_storage_preparations =
                this->protocol_memory_stats_.global_position_storage_preparations,
            .global_position_storage_allocations =
                this->protocol_memory_stats_.global_position_storage_allocations,
            .position_append_growth_reservations =
                this->protocol_memory_stats_.position_append_growth_reservations,
            .retained_position_count =
                static_cast<std::uint64_t>(this->protocol_global_position_count_),
            .retained_position_capacity =
                static_cast<std::uint64_t>(this->protocol_global_position_count_),
            .query_record_size_bytes =
                static_cast<std::uint8_t>(this->protocol_query_exact_counts_.width),
            .query_record_count = static_cast<std::uint64_t>(this->protocol_query_state_count_()),
            .query_record_capacity =
                static_cast<std::uint64_t>(this->protocol_query_state_capacity_()),
            .query_record_capacity_bytes = ledger.query_record_bytes,
            .query_record_position_capacity_bytes = ledger.query_record_position_bytes,
            .query_hash_capacity_bytes = query_hash_capacity_bytes,
            .released_query_hash_capacity_bytes =
                this->protocol_memory_stats_.released_query_hash_capacity_bytes,
            .query_route_radix_bits = this->protocol_query_radix_bits_,
            .query_route_radix_bucket_count =
                this->protocol_memory_stats_.query_route_radix_bucket_count,
            .query_route_radix_nonempty_bucket_count =
                this->protocol_memory_stats_.query_route_radix_nonempty_bucket_count,
            .query_route_radix_max_bucket_size =
                this->protocol_memory_stats_.query_route_radix_max_bucket_size,
            .query_route_control_lookups = this->protocol_memory_stats_.query_route_control_lookups,
            .query_route_hash_probes = this->protocol_memory_stats_.query_route_hash_probes,
            .query_route_matches = this->protocol_memory_stats_.query_route_matches,
            .decoded_control_records = this->protocol_memory_stats_.decoded_control_records,
            .retained_selected_control_records =
                this->protocol_memory_stats_.retained_selected_control_records,
            .selected_control_size_bytes = sizeof(typename protocol_core::selected_control),
            .retained_selected_control_capacity_bytes =
                this->protocol_memory_stats_.retained_selected_control_capacity_bytes,
            .current_selected_control_capacity_bytes = ledger.selected_extent_owner_bytes,
            .directory_capacity_bytes = ledger.directory_bytes,
            .ordinal_route_capacity_bytes = ledger.ordinal_map_bytes,
            .selected_extent_capacity_bytes = ledger.selected_extent_vector_bytes,
            .global_position_capacity_bytes = ledger.global_position_bytes,
            .control_payload_capacity_bytes = ledger.control_scratch_bytes,
            .owned_capacity_bytes = ledger.total_bytes,
            .saturated = this->protocol_memory_stats_.saturated,
        };
    }
};
static_assert(minimizer_lookup_index<prepared_grouped_soa_query_reader>);

} // namespace bioseq::indexing
