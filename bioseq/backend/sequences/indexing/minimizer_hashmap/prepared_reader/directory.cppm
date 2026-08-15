/**
 * @file directory.cppm
 * @brief Admitted grouped-SoA directory ownership and decoding.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:prepared_reader_directory;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader_receipts;
import bioseq.chaining.core;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.radix_sort;
import bioseq.common.seqan_adapter;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.instrumentation;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.storage.size;

export namespace bioseq::indexing {

/// Decoded grouped-SoA control with its half-open position interval.
struct grouped_soa_decoded_control {
    std::uint64_t hash = 0;
    std::uint64_t position_begin = 0;
    std::uint64_t position_end = 0;
    std::uint64_t saturated_count = 0;

    [[nodiscard]] bool operator==(const grouped_soa_decoded_control&) const = default;
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

} // namespace bioseq::indexing
