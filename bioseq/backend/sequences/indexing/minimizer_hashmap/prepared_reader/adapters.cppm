/**
 * @file adapters.cppm
 * @brief Public grouped-SoA prepared-reader lookup adapters.
 */
module;

#include <new>

export module bioseq.indexing.minimizer_hashmap:prepared_reader_adapters;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader_core_read_path;
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

/// Lookup result containing the global count and any retained positions.
struct grouped_soa_query_lookup {
    /// Total authenticated occurrences across every selected extent.
    std::uint64_t global_occurrence_count = 0;
    /// Whether all counted positions were retained under `max_occ`.
    bool positions_complete = true;
    /// Reader-owned positions, valid until the reader is destroyed.
    std::span<const std::uint64_t> positions{};
};

/// Reader-owned stats for the grouped-SoA cold-load memory path.  In
/// particular, `position_append_growth_reservations == 0` proves that payload
/// ingestion did not re-run live memory admission (and its OS probes) while
/// appending individual retained positions.
struct grouped_soa_query_reader_memory_stats {
    /// Number of full owned-capacity ledger reconciliations.
    std::uint64_t capacity_accounting_passes = 0;
    /// Number of attempts to allocate the final position array.
    std::uint64_t global_position_storage_preparations = 0;
    /// Number of successful final position-array allocations.
    std::uint64_t global_position_storage_allocations = 0;
    /// Admission requests made while appending decoded positions.
    std::uint64_t position_append_growth_reservations = 0;
    /// Number of authenticated positions retained for lookup.
    std::uint64_t retained_position_count = 0;
    /// Exact allocated array elements. For grouped-SoA this equals the retained
    /// count. The exported capacity name remains stable for E2E SQL.
    std::uint64_t retained_position_capacity = 0;
    /// Width of each compact query-state record in bytes.
    std::uint64_t query_record_size_bytes = 0;
    /// Number of live compact query-state records.
    std::uint64_t query_record_count = 0;
    /// Allocated compact query-state record count.
    std::uint64_t query_record_capacity = 0;
    /// Bytes owned by compact query-state records.
    std::uint64_t query_record_capacity_bytes = 0;
    /// Bytes owned by per-query position offsets.
    std::uint64_t query_record_position_capacity_bytes = 0;
    /// Exact bucket-implied query suffixes plus high-bit radix offsets. The
    /// aggregate SQL key retains this stable name and exact owner equation.
    std::uint64_t query_hash_capacity_bytes = 0;
    /// Query-routing bytes released after route construction.
    std::uint64_t released_query_hash_capacity_bytes = 0;
    /// Number of high hash bits used by the radix route.
    std::uint64_t query_route_radix_bits = 0;
    /// Number of radix buckets, including empty buckets.
    std::uint64_t query_route_radix_bucket_count = 0;
    /// Number of radix buckets containing at least one query.
    std::uint64_t query_route_radix_nonempty_bucket_count = 0;
    /// Largest query count assigned to one radix bucket.
    std::uint64_t query_route_radix_max_bucket_size = 0;
    /// Directory controls considered during query routing.
    std::uint64_t query_route_control_lookups = 0;
    /// Query hashes tested against routed control ranges.
    std::uint64_t query_route_hash_probes = 0;
    /// Query-to-control range matches retained by routing.
    std::uint64_t query_route_matches = 0;
    /// Control records decoded from accepted rows.
    std::uint64_t decoded_control_records = 0;
    /// Decoded controls retained because a query selected them.
    std::uint64_t retained_selected_control_records = 0;
    /// In-memory width of one selected-control record in bytes.
    std::uint64_t selected_control_size_bytes = 0;
    /// Peak bytes retained for selected-control records.
    std::uint64_t retained_selected_control_capacity_bytes = 0;
    /// Current bytes retained by selected-control ownership.
    std::uint64_t current_selected_control_capacity_bytes = 0;
    /// Bytes owned by encoded and decoded directory storage.
    std::uint64_t directory_capacity_bytes = 0;
    /// Bytes owned by directory-ordinal routing metadata.
    std::uint64_t ordinal_route_capacity_bytes = 0;
    /// Bytes owned by the selected-extent vector.
    std::uint64_t selected_extent_capacity_bytes = 0;
    /// Exact final position-array bytes, excluding allocator headroom.
    std::uint64_t global_position_capacity_bytes = 0;
    /// Bytes owned by control-payload decode scratch.
    std::uint64_t control_payload_capacity_bytes = 0;
    /// Sum of all currently owned capacities.
    std::uint64_t owned_capacity_bytes = 0;
    /// Whether any reported counter saturated.
    bool saturated = false;

    /// Return whether the component capacities sum to the owned-byte total.
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

    /// Begin a reader for a sorted unique 32-bit canonical query-hash set.
    [[nodiscard]] static std::expected<prepared_grouped_soa_query_reader,
                                       minimizer_generation_encoding_error>
    begin(bioseq::parallelism::memory_admission_controller& admission,
          const minimizer_generation_head& head,
          const minimizer_generation_manifest& manifest,
          const expected_minimizer_generation_identity& expected,
          std::span<const std::byte> encoded_directory,
          std::span<const std::uint32_t> query_hashes,
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
        if (!profile || profile->hash_width != minimizer_scalar_width::u32 ||
            !expected.max_occ_cap || *expected.max_occ_cap != profile->max_occ_cap ||
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
        if (!profile || !expected.max_occ_cap ||
            *expected.max_occ_cap != profile->max_occ_cap ||
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

    /// Legacy fixed-K15 canonical-query overload. The input must
    /// already be strictly sorted and unique by `kmer()`. It is validated while
    /// copying once into the reader-owned compact hash image, avoiding the
    /// caller's temporary vector, permit, and a second sort/unique.
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
        if (!profile || profile->hash_width != minimizer_scalar_width::u32 ||
            !expected.max_occ_cap || *expected.max_occ_cap != profile->max_occ_cap ||
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

    /// Return the authenticated generation identity without copying it.
    [[nodiscard]] const minimizer_generation_manifest& generation_manifest() const noexcept {
        return this->manifest_;
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
