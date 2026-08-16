/**
 * @file compact_direct/reader/contract.cppm
 * @brief Compact-direct reader state, nested contracts, and member declarations.
 */

export module bioseq.indexing.minimizer_hashmap:compact_direct.reader_contract;

import :compact_direct.query_contracts;
import :compact_direct.planning_admission;
import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
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

/// Fragment-native grouped-SoA reader.  Every physical position segment remains
/// an independently persisted mmap view. Seed collection probes fragment-local
/// controls and packed positions directly. Max-occ reduction is transient
/// query-wave work performed during that same fragment-parallel probe. Reader
/// open never scans or combines fragment controls. No posting list,
/// hash-to-fragment span directory, or target-wide occurrence table is
/// materialized, concatenated, merged, or sorted across fragments. The mapped
/// payload is the exact image emitted by PostgreSQL's complete authenticated
/// READY reread, so opening it validates identity and structure without
/// rereading fragment payload.  The compact-direct-only constructor is a
/// separate backing contract: it retains no grouped-SoA directory or payload and
/// authenticates uniform segment geometry without retaining scan-route
/// address plans.
class prepared_minimizer_fragment_reader {
  public:
    using fragment_seed_query_type = fragment_seed_query;
    using query_wave_scratch_type = query_wave_scratch;
    using prepared_fragment_anchor_batch_type = prepared_fragment_anchor_batch;

  private:
    static constexpr std::uint32_t query_radix_bits_ = 20u;
    static constexpr std::uint32_t query_radix_bucket_count_ = std::uint32_t{1}
                                                               << query_radix_bits_;
    static constexpr std::uint32_t query_radix_bucket_mask_ = query_radix_bucket_count_ - 1u;
    static constexpr std::uint32_t query_membership_filter_word_mask_ =
        (std::uint32_t{1} << 21u) - 1u;
    static constexpr std::size_t compact_direct_selection_shard_target_ = 32u;
    using occurrence_count_atomic = std::atomic<std::uint32_t>;
    using mapped_payload_owner_type = std::shared_ptr<const void>;
    using packed_occurrence_radix_partition = std::array<std::size_t, 257u>;

    struct compact_direct_selection_shard_range final {
        std::size_t begin = 0u;
        std::size_t end = 0u;
    };

    [[nodiscard]] static constexpr compact_direct_selection_shard_range
    compact_direct_selection_shard_range_(std::size_t selection_count,
                                          std::size_t shard_count,
                                          std::size_t shard_ordinal) noexcept {
        if (shard_count == 0u || shard_ordinal >= shard_count)
            return {};
        const std::size_t selections_per_shard = selection_count / shard_count;
        const std::size_t remainder = selection_count % shard_count;
        const std::size_t begin =
            shard_ordinal * selections_per_shard + std::min(shard_ordinal, remainder);
        return compact_direct_selection_shard_range{
            .begin = begin,
            .end = begin + selections_per_shard + (shard_ordinal < remainder ? 1u : 0u),
        };
    }

    enum class fragment_encoding_dispatch : std::uint8_t {
        checked_dynamic,
        h32_c16_p16,
    };

    enum class backing_kind : std::uint8_t {
        grouped_soa_fragments,
        compact_direct_only,
    };

    struct entry_plan {
        std::uint64_t control_begin = 0u;
        std::uint64_t control_end = 0u;
        std::uint64_t hash_begin = 0u;
        std::uint64_t hash_end = 0u;
        std::uint64_t count_begin = 0u;
        std::uint64_t count_end = 0u;
        std::uint64_t prefix_begin = 0u;
        std::uint64_t prefix_end = 0u;
        std::uint64_t position_begin = 0u;
        std::uint64_t position_end = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_element_count = 0u;
        std::uint64_t reference_begin = 0u;
        std::uint64_t reference_end = 0u;
        std::uint64_t control_scan_bytes = 0u;
        std::uint32_t position_segment = 0u;
        std::uint8_t hash_byte_width = 0u;
        std::uint8_t count_byte_width = 0u;
        std::uint8_t position_byte_width = 0u;
        std::uint8_t offset_byte_width = 0u;
        minimizer_scalar_width hash_width{};
        minimizer_scalar_width count_width{};
        minimizer_scalar_width position_width{};
        minimizer_scalar_width shape_offset_width{};
    };

    struct fragment_block_plan {
        std::uint32_t entry_begin = 0u;
        std::uint32_t entry_end = 0u;
        std::uint32_t first_position_segment = 0u;
        std::uint32_t last_position_segment = 0u;
        std::uint64_t reference_begin = 0u;
        std::uint64_t reference_end = 0u;
    };

    struct direct_open_contract {
        grouped_soa_minimizer_profile profile{};
        std::uint8_t hash_domain_bits = 0u;
    };

    bioseq::parallelism::memory_permit permit_{};
    grouped_soa_directory_view directory_{};
    minimizer_generation_head head_{};
    minimizer_generation_manifest manifest_{};
    grouped_soa_minimizer_profile profile_{};
    mapped_payload_owner_type mapped_payload_owner_{};
    std::span<const std::byte> mapped_payload_{};
    std::optional<owned_compact_hash32_direct_index_view> compact_direct_{};
    compact_direct_selected_page_adviser selected_page_adviser_{};
    std::unique_ptr<entry_plan[]> entry_plans_{};
    std::unique_ptr<fragment_block_plan[]> block_plans_{};
    std::size_t entry_count_ = 0;
    // Exact retained block_plans_ element count. Direct-only readers keep
    // this zero and expose manifest segment geometry through the public view.
    std::size_t block_count_ = 0;
    std::uint32_t largest_block_entry_count_ = 0u;
    std::uint32_t oversized_segment_block_count_ = 0u;
    std::uint64_t max_occ_ = 0;
    std::uint64_t total_control_hash_scan_bytes_ = 0u;
    std::uint64_t total_control_count_scan_bytes_ = 0u;
    std::uint64_t owned_bytes_ = 0;
    std::uint8_t query_radix_shift_ = 12u;
    std::uint8_t query_membership_filter_shift_ = 11u;
    std::uint16_t query_membership_filter_mask_id_mask_ = 0x7ffu;
    fragment_encoding_dispatch wire_dispatch_ = fragment_encoding_dispatch::checked_dynamic;
    backing_kind backing_kind_ = backing_kind::grouped_soa_fragments;
    bool ready_ = false;

    prepared_minimizer_fragment_reader() = default;

    [[nodiscard]] static constexpr std::uint32_t
    project_query_bucket_(std::uint32_t hash, std::uint8_t shift) noexcept {
        return (hash >> shift) & query_radix_bucket_mask_;
    }

    [[nodiscard]] constexpr std::uint32_t query_bucket_(std::uint32_t hash) const noexcept {
        return project_query_bucket_(hash, query_radix_shift_);
    }

    static_assert(((std::uint32_t{1} << 28u) >> 10u) == (std::uint32_t{1} << 18u));
    static_assert(((std::uint32_t{1} << 29u) >> 10u) == (std::uint32_t{1} << 19u));
    static_assert(((std::uint32_t{3} << 28u) >> 10u) == (std::uint32_t{3} << 18u));
    static_assert((std::numeric_limits<std::uint32_t>::max() >> 12u) ==
                  query_radix_bucket_mask_);

    [[nodiscard]] static std::uint64_t stage_now_ns_() noexcept {
        const auto now = bioseq::instrumentation::clock::now_ns();
        return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
    }

    [[nodiscard]] static std::uint64_t elapsed_stage_ns_(std::uint64_t started_ns) noexcept {
        const std::uint64_t finished_ns = stage_now_ns_();
        const std::uint64_t elapsed = finished_ns >= started_ns ? finished_ns - started_ns : 0u;
        return elapsed > 0u ? elapsed : 1u;
    }

    template <minimizer_scalar_width p_width>
    using minimizer_wire_uint_t_ = std::conditional_t<
        p_width == minimizer_scalar_width::u16,
        std::uint16_t,
        std::conditional_t<p_width == minimizer_scalar_width::u32, std::uint32_t, std::uint64_t>>;

    /// Read one scalar after open() has validated the containing component and
    /// extent geometry. The DEBUG_ASSERT documents that stats without
    /// replacing the release-active checks at the plan boundary.
    template <minimizer_scalar_width p_width>
    [[nodiscard, gnu::always_inline]]
    static std::uint64_t read_fixed_width_le_(std::span<const std::byte> validated_extent,
                                              std::size_t offset) noexcept {
        static_assert(p_width == minimizer_scalar_width::u16 ||
                      p_width == minimizer_scalar_width::u32 ||
                      p_width == minimizer_scalar_width::u64);
        using value_type = minimizer_wire_uint_t_<p_width>;
        DEBUG_ASSERT(offset <= validated_extent.size() &&
                         sizeof(value_type) <= validated_extent.size() - offset,
                     "fixed-width scalar read stays in its prevalidated extent");
        return minimizer_generation_wire_detail::read_le<value_type>(validated_extent, offset);
    }

    /// Concrete position decoder shared by both P3 occurrence routes. Route
    /// selection remains outside this cursor, so packed/wide does not become
    /// a template axis for the wire loop.
    class position_range_cursor {
      private:
        std::span<const std::byte> payload_{};
        grouped_soa_decoded_control control_{};
        minimizer_position_segment_bounds core_{};
        minimizer_scalar_width width_ = minimizer_scalar_width::u16;
        std::size_t position_begin_ = 0u;
        std::size_t position_byte_width_ = 0u;
        std::uint64_t ordinal_ = 0u;
        std::optional<std::uint64_t> previous_{};
        std::optional<minimizer_generation_encoding_error> error_{};

        [[nodiscard]] bool accept_local_(std::uint64_t local,
                                         std::uint64_t& packed_global) noexcept {
            const std::uint64_t local_bp = local >> 1u;
            if (core_.end_bp <= core_.begin_bp || local_bp >= core_.end_bp - core_.begin_bp ||
                core_.begin_bp > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
                error_ = minimizer_generation_encoding_error::position_range;
                return false;
            }
            const std::uint64_t global_bp = core_.begin_bp + local_bp;
            if (global_bp > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
                error_ = minimizer_generation_encoding_error::arithmetic_overflow;
                return false;
            }
            packed_global = (global_bp << 1u) | (local & 1u);
            if (previous_ && packed_global <= *previous_) {
                error_ = minimizer_generation_encoding_error::position_range;
                return false;
            }
            previous_ = packed_global;
            ++ordinal_;
            return true;
        }

      public:
        [[nodiscard]] static std::expected<position_range_cursor,
                                           minimizer_generation_encoding_error>
        create(std::span<const std::byte> payload,
               const grouped_soa_decoded_control& control,
               const entry_plan& entry) noexcept {
            if (control.position_end <= control.position_begin ||
                control.position_end > entry.position_element_count) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            DEBUG_ASSERT(is_minimizer_scalar_width(entry.position_width) &&
                             entry.position_byte_width ==
                                 static_cast<std::uint8_t>(entry.position_width) &&
                             entry.position_begin <= entry.position_end &&
                             entry.position_end <= payload.size() &&
                             entry.position_end - entry.position_begin ==
                                 entry.position_element_count * entry.position_byte_width,
                         "position cursor consumes open-validated entry geometry");
            position_range_cursor cursor;
            cursor.payload_ = payload;
            cursor.control_ = control;
            cursor.core_ = minimizer_position_segment_bounds{
                .begin_bp = entry.reference_begin,
                .end_bp = entry.reference_end,
            };
            cursor.width_ = entry.position_width;
            cursor.position_begin_ = static_cast<std::size_t>(entry.position_begin);
            cursor.position_byte_width_ = entry.position_byte_width;
            cursor.ordinal_ = control.position_begin;
            return cursor;
        }

        [[nodiscard, gnu::always_inline]] bool
        next_fixed_p16(std::uint64_t& packed_global) noexcept {
            if (error_ || ordinal_ >= control_.position_end)
                return false;
            DEBUG_ASSERT(width_ == minimizer_scalar_width::u16,
                         "fixed P16 cursor requires a P16 plan");
            const std::size_t offset =
                position_begin_ + static_cast<std::size_t>(ordinal_) * sizeof(std::uint16_t);
            return accept_local_(
                read_fixed_width_le_<minimizer_scalar_width::u16>(payload_, offset), packed_global);
        }

        [[nodiscard, gnu::always_inline]] bool next_dynamic(std::uint64_t& packed_global) noexcept {
            if (error_ || ordinal_ >= control_.position_end)
                return false;
            const std::size_t offset =
                position_begin_ + static_cast<std::size_t>(ordinal_) * position_byte_width_;
            std::uint64_t local = 0u;
            switch (width_) {
            case minimizer_scalar_width::u16:
                local = minimizer_generation_wire_detail::read_le<std::uint16_t>(payload_, offset);
                break;
            case minimizer_scalar_width::u32:
                local = minimizer_generation_wire_detail::read_le<std::uint32_t>(payload_, offset);
                break;
            case minimizer_scalar_width::u64:
                local = minimizer_generation_wire_detail::read_le<std::uint64_t>(payload_, offset);
                break;
            }
            return accept_local_(local, packed_global);
        }

        [[nodiscard]] std::optional<minimizer_generation_encoding_error> error() const noexcept {
            return error_;
        }
    };

    enum class fragment_probe_pass : std::uint8_t {
        control_count,
        rejected_validation,
        orientation,
        emission,
    };

    struct query_occurrence_group final {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t counter_ordinal = 0u;
    };
    static_assert(sizeof(query_occurrence_group) == 16u);

    struct bucket_direct_matcher_view final {
        const std::uint64_t* membership_filter = nullptr;
        const std::uint32_t* radix = nullptr;
        const std::uint32_t* group_offsets = nullptr;
        const packed_query_occurrence* packed_occurrences = nullptr;
        const wide_query_occurrence* wide_occurrences = nullptr;
        occurrence_count_atomic* occurrence_counts = nullptr;
        std::size_t membership_filter_word_count = 0u;
        std::size_t radix_count = 0u;
        std::size_t group_offset_count = 0u;
        std::size_t packed_occurrence_count = 0u;
        std::size_t wide_occurrence_count = 0u;
        std::size_t occurrence_count = 0u;
        std::size_t counter_count = 0u;
        query_occurrence_packing_layout occurrence_layout{};
    };

    struct fragment_probe_context final {
        fragment_probe_pass pass = fragment_probe_pass::control_count;
        const fragment_block_plan* block = nullptr;
        std::size_t block_ordinal = 0u;
        std::span<const query_wave_scratch::query_hash_plan> plans{};
        std::span<const std::uint32_t> query_plan_radix{};
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        const bucket_direct_matcher_view* bucket_direct_matcher = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::uint32_t max_occ = 0u;
        std::uint64_t* matched_count_bytes = nullptr;
        std::uint64_t* accepted_prefix_bytes = nullptr;
        std::uint64_t* rejected_prefix_bytes = nullptr;
        std::uint64_t* position_bytes = nullptr;
        std::uint64_t* anchors_constructed = nullptr;
    };

    struct alignas(64) compact_direct_layout_pass_context final {
        const prepared_minimizer_fragment_reader* reader = nullptr;
        const compact_hash32_unique_query_plan* plan = nullptr;
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        std::size_t shard_ordinal = 0u;
        std::uint64_t logical_postings_counted = 0u;
        compact_direct_selected_page_advice_accumulator selected_page_advice{};
        std::optional<minimizer_generation_encoding_error> error{};
        std::optional<direct_inverted_index_error> direct_error{};
        std::optional<compact_hash32_selection_span_visit_stats> visit_stats{};
    };

    struct alignas(64) compact_direct_emission_pass_context final {
        const prepared_minimizer_fragment_reader* reader = nullptr;
        const compact_hash32_unique_query_plan* plan = nullptr;
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::size_t shard_ordinal = 0u;
        std::uint64_t posting_bytes_read = 0u;
        std::uint64_t postings_visited = 0u;
        std::uint64_t anchors_constructed = 0u;
        std::optional<minimizer_generation_encoding_error> error{};
        std::optional<direct_inverted_index_error> direct_error{};
        std::optional<compact_hash32_selection_span_visit_stats> visit_stats{};
    };

    inline static constexpr std::array<std::uint64_t, 1u << 11u> query_membership_filter_masks_ =
        []() consteval {
            std::array<std::uint64_t, 1u << 11u> masks{};
            for (std::size_t low = 0u; low < masks.size(); ++low) {
                const std::uint64_t mixed = (static_cast<std::uint64_t>(low) + 1u) *
                                            std::uint64_t{0x9e3779b97f4a7c15};
                masks[low] = (std::uint64_t{1} << ((mixed >> 0u) & 63u)) |
                             (std::uint64_t{1} << ((mixed >> 6u) & 63u)) |
                             (std::uint64_t{1} << ((mixed >> 12u) & 63u)) |
                             (std::uint64_t{1} << ((mixed >> 18u) & 63u)) |
                             (std::uint64_t{1} << ((mixed >> 24u) & 63u)) |
                             (std::uint64_t{1} << ((mixed >> 30u) & 63u));
            }
            return masks;
        }();

    [[nodiscard, gnu::always_inline]] constexpr std::uint64_t
    query_membership_filter_mask_(std::uint32_t hash) const noexcept {
        // Reader open derives the low-bit mask from the persisted hash domain.
        // Those remaining bits identify a hash inside one filter word; one
        // compile-time LUT preserves the exact six register-blocked positions
        // without repeating their multiplicative diffusion in every probe.
        // A query inserts this exact mask and a probe tests this exact mask,
        // which is the no-false-negative authority; positives still undergo
        // an exact hash comparison inside their packed-occurrence bucket.
        return query_membership_filter_masks_[hash & query_membership_filter_mask_id_mask_];
    }

    [[nodiscard]] static constexpr std::size_t
    project_query_membership_filter_word_(std::uint32_t hash, std::uint8_t shift) noexcept {
        return static_cast<std::size_t>((hash >> shift) & query_membership_filter_word_mask_);
    }

    [[nodiscard, gnu::always_inline]] constexpr std::size_t
    query_membership_filter_word_(std::uint32_t hash) const noexcept {
        return project_query_membership_filter_word_(hash, query_membership_filter_shift_);
    }

    static_assert(((std::uint32_t{1} << 28u) >> 9u) == (std::uint32_t{1} << 19u));
    static_assert(((std::uint32_t{1} << 29u) >> 9u) == (std::uint32_t{1} << 20u));
    static_assert(((std::uint32_t{3} << 28u) >> 9u) == (std::uint32_t{3} << 19u));
    static_assert((std::numeric_limits<std::uint32_t>::max() >> 11u) ==
                  query_membership_filter_word_mask_);

    [[nodiscard, gnu::always_inline]] static query_occurrence_reference
    query_occurrence_reference_(const packed_query_occurrence& occurrence,
                                const query_occurrence_packing_layout& layout) noexcept {
        return decode_packed_query_occurrence_trusted(layout, occurrence.query_and_ordinal);
    }

    [[nodiscard, gnu::always_inline]] static query_occurrence_reference
    query_occurrence_reference_(const wide_query_occurrence& occurrence,
                                const query_occurrence_packing_layout&) noexcept {
        return query_occurrence_reference{
            .batch_query_ordinal = occurrence.batch_query_ordinal,
            .query_ordinal = occurrence.query_ordinal,
        };
    }

    /// Select packed or wide storage once for a complete hash plan. The
    /// visitor receives a route-specific span, so its target-position x query-
    /// occurrence loop contains no representation branch.
    template <bool p_oracle, class p_visitor>
    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    with_query_group_occurrences_(const fragment_probe_context& context,
                                  const query_occurrence_group& group,
                                  p_visitor&& visitor) noexcept;

    template <bool p_oracle>
    [[nodiscard, gnu::always_inline]] static std::atomic<std::uint32_t>&
    occurrence_counter_(const fragment_probe_context& context,
                        std::uint32_t counter_ordinal) noexcept {
        if constexpr (p_oracle) {
            DEBUG_ASSERT(context.scratch != nullptr &&
                             counter_ordinal < context.scratch->active_occurrence_count_,
                         "oracle counter ordinal is prevalidated");
            return context.scratch->occurrence_counts_[counter_ordinal];
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr &&
                             counter_ordinal < context.bucket_direct_matcher->counter_count,
                         "production counter ordinal is prevalidated");
            return context.bucket_direct_matcher->occurrence_counts[counter_ordinal];
        }
    }

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    add_expected_anchor_count_(query_occurrence_reference occurrence,
                               std::span<const fragment_seed_query> queries,
                               prepared_fragment_anchor_batch& batch,
                               std::uint32_t global_count) noexcept {
        const std::uint32_t query_ordinal = occurrence.batch_query_ordinal;
        if (query_ordinal >= queries.size() ||
            !add_checked_(batch.queries_[query_ordinal].anchor_count, global_count)) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return std::nullopt;
    }

    struct orientation_occurrence_context {
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        std::size_t layout_base = 0u;
        std::uint64_t target_strand_zero = 0u;
        std::uint64_t target_strand_one = 0u;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    accumulate_orientation_(query_occurrence_reference occurrence,
                            const orientation_occurrence_context& context) noexcept;

    struct direct_emission_occurrence_context {
        std::span<const fragment_seed_query> queries{};
        const fragment_block_plan* block = nullptr;
        std::size_t block_ordinal = 0u;
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    emit_fragment_anchor_(std::uint64_t packed_target_position,
                          query_occurrence_reference occurrence,
                          const direct_emission_occurrence_context& context) noexcept;

    struct compact_direct_emission_occurrence_context final {
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::size_t layout_base = 0u;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    emit_compact_direct_anchor_(std::uint64_t packed_target_position,
                                query_occurrence_reference occurrence,
                                const compact_direct_emission_occurrence_context& context) noexcept;

    template <class p_context>
    [[nodiscard]] static bool
    bind_compact_direct_selection_(p_context& context,
                                   const compact_hash32_selection_posting_spans& selected,
                                   query_occurrence_group& group) noexcept;

    static void flush_compact_direct_selected_page_advice_(
        compact_direct_layout_pass_context& context) noexcept {
        if (context.reader == nullptr)
            return;
        context.selected_page_advice.flush(context.reader->selected_page_adviser_);
    }

    [[nodiscard]] static bool accumulate_compact_direct_selected_page_advice_(
        compact_direct_layout_pass_context& context,
        const compact_hash32_selection_posting_spans& selected) noexcept;

    [[nodiscard]] static bool visit_compact_direct_layout_selection_(
        compact_direct_layout_pass_context& context,
        const compact_hash32_selection_posting_spans& selected) noexcept;

    [[nodiscard]] static bool visit_compact_direct_emission_selection_(
        compact_direct_emission_pass_context& context,
        const compact_hash32_selection_posting_spans& selected) noexcept;

    /// Reserve one fragment-local occurrence count in a query-wave counter.
    /// A value of max_occ + 1 is the only rejected state. This bounds decoded
    /// candidates for a globally repetitive hash to at most max_occ while
    /// preserving every hit for hashes whose final target-wide count is
    /// accepted. The counter is query state, never persisted minimizer-fragment index state.
    [[nodiscard]] static bool reserve_query_occurrences_(std::atomic<std::uint32_t>& global_count,
                                                         std::uint64_t local_count,
                                                         std::uint32_t max_occ) noexcept;

    static void record_error_(std::atomic<std::uint32_t>& destination,
                              minimizer_generation_encoding_error error) noexcept {
        std::uint32_t expected = 0u;
        const std::uint32_t encoded = static_cast<std::uint32_t>(error) + 1u;
        (void)destination.compare_exchange_strong(
            expected, encoded, std::memory_order_relaxed, std::memory_order_relaxed);
    }

    [[nodiscard]] static bool add_atomic_checked_(std::atomic<std::uint64_t>& destination,
                                                  const std::uint64_t increment) noexcept {
        std::uint64_t current = destination.load(std::memory_order_relaxed);
        for (;;) {
            if (increment > (std::numeric_limits<std::uint64_t>::max)() - current) {
                return false;
            }
            if (destination.compare_exchange_weak(current,
                                                  current + increment,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    recorded_error_(const std::atomic<std::uint32_t>& source) noexcept {
        const std::uint32_t encoded = source.load(std::memory_order_relaxed);
        return encoded == 0u ? std::nullopt
                             : std::optional<minimizer_generation_encoding_error>{
                                   static_cast<minimizer_generation_encoding_error>(encoded - 1u)};
    }

    template <class p_body> static void parallel_entries_(std::size_t count, p_body&& body) {
        bioseq::parallelism::parallel_for_pipeline(
            0u,
            count,
            1u,
            bioseq::parallelism::task_parallel_minimums::k_fragment_count,
            bioseq::parallelism::task_parallel_region::fragment_build,
            std::forward<p_body>(body));
    }

    /// Execute exactly the first (most-significant) byte pass of the ordinary
    /// minimap-style u32 radix and return its 256 disjoint bucket extents.
    /// The returned state is constant-size stack data. No occurrence owner or
    /// per-query merge is introduced.  Lower-byte sorts may therefore run in
    /// any order without changing the serial radix's byte-for-byte result.
    [[nodiscard]] static packed_occurrence_radix_partition
    partition_packed_occurrence_top_byte_(std::span<packed_query_occurrence> occurrences) noexcept;

    /// Count-only control scan over one open-validated entry plan. It
    /// deliberately does not load a prefix scalar or position byte. Accepted
    /// plans receive their range validation in passes 2 and 3. All component
    /// offsets, widths, and byte products were validated once by open().
    template <class p_visitor>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    visit_control_counts_planned_(std::size_t entry_ordinal, p_visitor&& visitor) const noexcept;

    template <class p_visitor>
    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    visit_controls_(std::span<const std::byte> input,
                    const grouped_soa_bucket_directory_entry& entry,
                    const grouped_soa_minimizer_profile& profile,
                    p_visitor&& visitor);

    [[nodiscard, gnu::always_inline]]
    static std::uint64_t read_shape_scalar_trusted_(std::span<const std::byte> input,
                                                    std::size_t offset,
                                                    minimizer_scalar_width width) noexcept {
        using namespace minimizer_generation_wire_detail;
        switch (width) {
        case minimizer_scalar_width::u16:
            return read_le<std::uint16_t>(input, offset);
        case minimizer_scalar_width::u32:
            return read_le<std::uint32_t>(input, offset);
        case minimizer_scalar_width::u64:
            return read_le<std::uint64_t>(input, offset);
        }
        std::unreachable();
    }

    template <class p_visitor>
    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    visit_position_range_(std::span<const std::byte> input,
                          const grouped_soa_decoded_control& control,
                          const grouped_soa_bucket_directory_entry& entry,
                          const minimizer_position_segment_bounds& core,
                          const grouped_soa_minimizer_profile& profile,
                          p_visitor&& visitor) noexcept;

    [[nodiscard]] static bool add_checked_(std::uint64_t& destination,
                                           std::uint64_t value) noexcept {
        if (destination > (std::numeric_limits<std::uint64_t>::max)() - value) {
            return false;
        }
        destination += value;
        return true;
    }

    /// Delegate both ordinary and fragment-native ordering to the same
    /// duplicate-linear authority and return exact payload-work receipts for
    /// independently timed primary and tie stages.
    [[nodiscard]] static bioseq::chaining::canonical_anchor_sort_stats
    sort_anchor_slice_primary_(bioseq::chaining::anchor* anchors,
                               std::size_t anchor_count,
                               std::uint64_t authenticated_target_length) noexcept {
        return bioseq::chaining::sort_canonical_anchor_primary_bounded(
            std::span<bioseq::chaining::anchor>{anchors, anchor_count},
            authenticated_target_length);
    }

    [[nodiscard]] static bioseq::chaining::canonical_anchor_sort_stats
    normalize_anchor_slice_ties_(bioseq::chaining::anchor* anchors,
                                 std::size_t anchor_count) noexcept {
        return bioseq::chaining::normalize_canonical_anchor_ties(
            std::span<bioseq::chaining::anchor>{anchors, anchor_count});
    }

    [[nodiscard]] static std::expected<bioseq::chaining::anchor,
                                       minimizer_generation_encoding_error>
    make_fragment_anchor_(std::uint64_t packed_target,
                          const minimap_sketch_data& minimizer,
                          sequence_size_t query_length,
                          std::uint8_t query_span) noexcept;

    template <bool p_oracle, class p_visitor>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    visit_matched_control_counts_(const fragment_block_plan& block,
                                  fragment_probe_context& context,
                                  p_visitor&& visitor) const noexcept;

    template <bool p_oracle, class p_visitor>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    visit_matched_controls_(const fragment_block_plan& block,
                            fragment_probe_context& context,
                            p_visitor&& visitor,
                            std::uint64_t* accepted_prefix_bytes = nullptr,
                            std::uint64_t* rejected_prefix_bytes = nullptr,
                            bool validate_rejected_prefixes = true) const noexcept;

    template <bool p_oracle>
    [[nodiscard, gnu::always_inline]]
    std::optional<query_occurrence_group>
    match_query_group_(std::uint32_t hash,
                       const fragment_probe_context& context,
                       std::uint32_t& active_bucket,
                       std::uint32_t& plan_cursor,
                       std::uint32_t& plan_end) const noexcept;

    template <bool p_oracle>
    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    accumulate_orientation_for_group_(const fragment_probe_context& context,
                                      const query_occurrence_group& group,
                                      std::uint64_t target_strand_zero,
                                      std::uint64_t target_strand_one) noexcept;

    template <bool p_oracle>
    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    process_fixed_orientation_control_(fragment_probe_context& context,
                                       const entry_plan& entry,
                                       const query_occurrence_group& group,
                                       const grouped_soa_decoded_control& control) const noexcept;

    template <bool p_oracle>
    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    process_fixed_emission_control_(fragment_probe_context& context,
                                    const entry_plan& entry,
                                    const query_occurrence_group& group,
                                    const grouped_soa_decoded_control& control) const noexcept;

    template <bool p_oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_count_(fragment_probe_context& context,
                               std::size_t entry_ordinal) const noexcept;

    template <bool p_oracle, minimizer_scalar_width p_offset_width>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_p16_matched_(fragment_probe_context& context,
                                     std::size_t entry_ordinal) const noexcept;

    template <bool p_oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_p16_dispatch_(fragment_probe_context& context,
                                      std::size_t entry_ordinal) const noexcept {
        if (context.pass == fragment_probe_pass::control_count) {
            return probe_entry_h32_c16_count_<p_oracle>(context, entry_ordinal);
        }
        switch (entry_plans_[entry_ordinal].shape_offset_width) {
        case minimizer_scalar_width::u16:
            return probe_entry_h32_c16_p16_matched_<p_oracle, minimizer_scalar_width::u16>(
                context, entry_ordinal);
        case minimizer_scalar_width::u32:
            return probe_entry_h32_c16_p16_matched_<p_oracle, minimizer_scalar_width::u32>(
                context, entry_ordinal);
        case minimizer_scalar_width::u64:
            return probe_entry_h32_c16_p16_matched_<p_oracle, minimizer_scalar_width::u64>(
                context, entry_ordinal);
        }
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }

    template <bool p_oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_block_h32_c16_p16_(fragment_probe_context& context) const noexcept {
        if (context.block == nullptr || context.block->entry_begin >= context.block->entry_end ||
            context.block->entry_end > entry_count_) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        for (std::size_t entry_ordinal = context.block->entry_begin;
             entry_ordinal < context.block->entry_end;
             ++entry_ordinal) {
            const auto probed = probe_entry_h32_c16_p16_dispatch_<p_oracle>(context, entry_ordinal);
            if (!probed)
                return probed;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t final_owned_bytes_() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = directory_.owned_capacity_bytes();
        bytes = saturating_add(bytes, saturating_mul(entry_count_, sizeof(entry_plan)));
        bytes = saturating_add(bytes, saturating_mul(block_count_, sizeof(fragment_block_plan)));
        return bytes;
    }

    [[nodiscard]] static std::expected<direct_open_contract, minimizer_generation_encoding_error>
    validate_direct_open_contract_(const minimizer_generation_head& head,
                                   const minimizer_generation_manifest& manifest,
                                   const expected_minimizer_generation_identity& expected,
                                   std::uint64_t max_occ,
                                   const owned_compact_hash32_direct_index_view& compact_direct);

    /// Materialize every planned query occurrence and establish its route-specific hash order.
    template <bool p_oracle, bool p_compact_direct>
    [[gnu::always_inline]] void prepare_query_occurrences_(
        std::span<const fragment_seed_query> queries,
        const std::expected<query_occurrence_packing_layout,
                            minimizer_generation_encoding_error>& occurrence_layout,
        std::size_t occurrence_count,
        query_wave_scratch& scratch,
        prepared_fragment_anchor_batch& batch) const;

    void
    prepare_compact_direct_anchors_(bioseq::parallelism::memory_admission_controller& admission,
                                    std::span<const fragment_seed_query> queries,
                                    std::uint32_t runtime_max_occ,
                                    query_wave_scratch& scratch,
                                    prepared_fragment_anchor_batch& batch) const;

  public:
    prepared_minimizer_fragment_reader(prepared_minimizer_fragment_reader&&) noexcept = default;
    prepared_minimizer_fragment_reader& operator=(prepared_minimizer_fragment_reader&&) = delete;
    prepared_minimizer_fragment_reader(const prepared_minimizer_fragment_reader&) = delete;
    prepared_minimizer_fragment_reader&
    operator=(const prepared_minimizer_fragment_reader&) = delete;

    /// Open the compact-H32 route without decoding or retaining the grouped-SoA
    /// directory and without owning a grouped-SoA payload mapping.  The manifest
    /// supplies only its authenticated uniform segment geometry. The direct
    /// image remains the sole posting/control authority for this reader.
    [[nodiscard]] static std::expected<prepared_minimizer_fragment_reader,
                                       minimizer_generation_encoding_error>
    open_compact_direct(bioseq::parallelism::memory_admission_controller& admission,
                        const minimizer_generation_head& head,
                        const minimizer_generation_manifest& manifest,
                        const expected_minimizer_generation_identity& expected,
                        std::uint64_t max_occ,
                        owned_compact_hash32_direct_index_view compact_direct,
                        compact_direct_selected_page_adviser selected_page_adviser = {});

    [[nodiscard]] static std::expected<prepared_minimizer_fragment_reader,
                                       minimizer_generation_encoding_error>
    open(bioseq::parallelism::memory_admission_controller& admission,
         const minimizer_generation_head& head,
         const minimizer_generation_manifest& manifest,
         const expected_minimizer_generation_identity& expected,
         std::span<const std::byte> encoded_directory,
         std::uint64_t max_occ,
         std::shared_ptr<const void> mapped_payload_owner,
         std::span<const std::byte> mapped_payload,
         std::optional<owned_compact_hash32_direct_index_view> compact_direct = std::nullopt);

    /// Prepare one immutable, canonically ordered anchor span per query.
    /// grouped-SoA control scans use count/layout/emission passes. The explicit
    /// compact-direct route instead performs one lookup per unique query hash,
    /// derives exact query/orientation counts from accepted descriptors, and
    /// decodes each accepted posting exactly once into its final owner, with no
    /// control-scan fallback.
    [[nodiscard]] prepared_fragment_anchor_batch prepare_fragment_anchor_batch(
        bioseq::parallelism::memory_admission_controller& admission,
        std::span<const fragment_seed_query> queries,
        std::int32_t max_occ,
        query_wave_scratch& scratch,
        bool baseline_query_plan_matcher_for_testing = false,
        bool verify_compositional_order_for_testing = false,
        minimizer_fragment_lookup_route lookup_route =
            minimizer_fragment_lookup_route::grouped_soa_control_scans) const;

    template <bool p_oracle, bool p_compact_direct>
    [[nodiscard]] prepared_fragment_anchor_batch
    prepare_fragment_anchor_batch_impl_(bioseq::parallelism::memory_admission_controller& admission,
                                        std::span<const fragment_seed_query> queries,
                                        std::int32_t max_occ,
                                        query_wave_scratch& scratch,
                                        bool verify_compositional_order_for_testing) const;

    [[nodiscard]] bool ready() const noexcept {
        return ready_;
    }

    [[nodiscard]] bool compact_direct_ready() const noexcept {
        return ready_ && compact_direct_.has_value();
    }

    [[nodiscard]] bool compact_direct_only() const noexcept {
        return ready_ && backing_kind_ == backing_kind::compact_direct_only;
    }

    [[nodiscard]] std::size_t mapped_grouped_soa_payload_bytes() const noexcept {
        return mapped_payload_.size();
    }

    [[nodiscard]] std::size_t grouped_soa_directory_entry_count() const noexcept {
        return entry_count_;
    }

    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return manifest_.target_length;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] std::size_t fragment_block_count() const noexcept {
        // Preserve the direct reader's manifest-geometry observation without
        // representing those segments as retained fragment_block_plan owners.
        return backing_kind_ == backing_kind::compact_direct_only ? manifest_.position_segment_count
                                                                  : block_count_;
    }

    [[nodiscard]] std::uint32_t largest_fragment_block_entry_count() const noexcept {
        return largest_block_entry_count_;
    }

    [[nodiscard]] std::uint32_t oversized_segment_block_count() const noexcept {
        return oversized_segment_block_count_;
    }

    [[nodiscard]] grouped_soa_query_reader_memory_stats memory_stats() const noexcept;
};

static_assert(prepared_fragment_anchor_batch_reader<prepared_minimizer_fragment_reader>);
static_assert(prepared_fragment_anchor_query_index<prepared_fragment_anchor_batch::query_handle>);
static_assert(!minimizer_lookup_index<prepared_minimizer_fragment_reader>);

} // namespace bioseq::indexing
