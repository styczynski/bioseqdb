/**
 * @file declarations.cppm
 * @brief Declares prepared query wire types, ownership state, and protocol operations.
 */
export module bioseq.indexing.minimizer_hashmap:prepared_reader_core_read_path_declarations;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader_directory;
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

namespace minimizer_prepared_reader_detail {

/**
 * @brief Provides wire operations specialized by authenticated storage layout.
 * @tparam p_traits Storage-layout traits selecting the wire representation.
 */
template <class p_traits> struct prepared_query_wire_ops;

/**
 * @brief Stores prepared state with layout-selected position ownership.
 * @tparam p_hash Hash value type.
 * @tparam p_flat_position_image Whether positions use one shared flat image.
 */
template <class p_hash, bool p_flat_position_image> struct prepared_query_record;

/**
 * @brief Stores query state beside one shared flat position image.
 * @tparam p_hash Hash value type.
 */
template <class p_hash> struct prepared_query_record<p_hash, true> {
    std::uint64_t global_occurrence_count = 0;
    std::uint64_t global_position_offset = 0;
    std::uint64_t global_positions_written = 0;
    bool positions_complete = true;
};

/**
 * @brief Stores query state with a per-query retained position vector.
 * @tparam p_hash Hash value type.
 */
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

/**
 * @brief Owns admitted routing, extent progress, occurrence policy, and retained positions.
 * @tparam p_traits Authenticated storage-layout and codec traits.
 */
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

    /**
     * @brief Records the first protocol failure and releases admitted storage.
     * @tparam p_value Expected result value type.
     * @param[in] error Failure retained for subsequent protocol calls.
     * @return The retained failure as an unexpected result.
     */
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

    /// Admits and allocates the exact retained-position image after control decoding.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    prepare_global_position_storage();

    static constexpr std::size_t query_radix_target_mean_ = 4u;

    /// Describes the admitted high-bit query directory and retained suffix width.
    struct query_radix_plan {
        std::uint32_t hash_bits = 0u;
        std::uint32_t radix_bits = 0u;
        std::uint32_t suffix_bits = 0u;
        prepared_query_scalar_width suffix_width = prepared_query_scalar_width::u8;
        std::size_t bucket_count = 1u;
        std::size_t offset_count = 2u;
    };

    /// Sizes the high-bit directory for bounded exact point-lookup buckets.
    [[nodiscard]] static std::expected<query_radix_plan, minimizer_generation_encoding_error>
    plan_query_radix_(std::size_t query_count, std::uint64_t max_hash) noexcept;

    /// Maps a query hash to its high-bit directory bucket.
    [[nodiscard]] static std::size_t
    query_radix_bucket_(hash_type hash, std::uint32_t hash_bits, std::uint32_t radix_bits) noexcept;

    /// Extracts the suffix retained inside one query directory bucket.
    [[nodiscard]] static std::uint64_t
    query_radix_suffix_(hash_type hash, std::uint32_t suffix_bits) noexcept;

    /// Resolves one hash while optionally counting exact-route probes.
    [[nodiscard]] std::size_t
    query_index_for_hash_impl_(std::uint64_t hash, std::uint32_t* hash_probes) const noexcept;

    [[nodiscard]] std::size_t query_index_for_hash(std::uint64_t hash) const noexcept;

    /// Resolves a control hash and reports its exact-route probe count.
    [[nodiscard]] std::size_t
    query_index_for_control_hash(std::uint64_t hash, std::uint32_t& hash_probes) const noexcept;

    /// Reports whether a directory entry overlaps the sorted query set.
    [[nodiscard]] bool
    query_intersects_entry(const grouped_soa_bucket_directory_entry& entry) const noexcept;

    /// Positions the physical reader at the first authenticated extent.
    void initialize_physical_cursor() noexcept;

    /// Advances the physical reader across control and position stages.
    void advance_physical_cursor() noexcept;

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    initialize_identity_core(bioseq::parallelism::memory_admission_controller& admission,
                             const minimizer_generation_head& head,
                             const minimizer_generation_manifest& manifest,
                             const expected_minimizer_generation_identity& expected,
                             const grouped_soa_minimizer_profile& profile,
                             std::span<const std::byte> encoded_directory,
                             std::uint64_t additional_logical_bytes = 0);

    [[nodiscard]] static constexpr std::uint64_t query_hash_value_(std::uint64_t hash) noexcept;

    [[nodiscard]] static std::uint64_t
    query_hash_value_(const minimap_sketch_data& minimizer) noexcept;

    /**
     * @brief Admits and assembles query routes from authenticated generation metadata.
     * @tparam p_query_value Query scalar or minimizer sketch type.
     */
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
                              bool query_hashes_are_sorted_unique = false);

    /// Admits reusable storage for the largest selected control extent.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    ensure_control_scratch_();

    /// Decodes and routes one authenticated control extent.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    observe_control_extent(const grouped_soa_bucket_directory_entry& entry,
                           std::span<const std::byte> bytes);

    /// Appends one decoded position to its admitted query owner.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    append_retained_position(std::size_t query_index, std::uint64_t value);

    /// Decodes and routes one authenticated position payload.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    observe_position_payload(const grouped_soa_bucket_directory_entry& entry,
                             std::span<const std::byte> payload);

    /// Validates and consumes one row under the selected checksum authority.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row_impl(const minimizer_generation_row_view& row,
                             bool row_checksum_prevalidated);

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row(const minimizer_generation_row_view& row);

    /// Consumes one row carrying validated checksum authority.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    accept_protocol_row(const validated_minimizer_generation_row_receipt& receipt);

    /// Finalizes decoded extents and publishes lookup-ready state.
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    finish_query_protocol();

    /// Borrows one prepared query result and its retained positions.
    struct protocol_lookup {
        std::uint64_t global_occurrence_count = 0;
        bool positions_complete = true;
        std::span<const std::uint64_t> positions{};
    };

    /// Looks up one hash after successful protocol finalization.
    [[nodiscard]] protocol_lookup lookup_protocol(std::uint64_t hash) const noexcept;

    /// Counts retained positions across the active storage layout.
    [[nodiscard]] std::size_t protocol_retained_position_count() const noexcept;

    [[nodiscard]] const grouped_soa_query_admission_stats&
    protocol_admission_failure_stats() const noexcept;
};

} // namespace minimizer_prepared_reader_detail

} // namespace bioseq::indexing
