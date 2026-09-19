/**
 * @file generation_session_contract.cppm
 * @brief Declares grouped-generation read contracts, ownership, and inline sink templates.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/htup.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "executor/instrument.h"
#include "executor/tuptable.h"
#include "storage/itemptr.h"
#include "utils/fmgroids.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:generation_session_contract;
import std;
export import bioseq.postgres.storage.indexing.minimizer_read_stats;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // the concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // canonical bounded base decoding
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.instrumentation.clock;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import :catalog;
import :catalog_row_io;
import :fragment_image;
export namespace bioseq::storage::indexing {
/** Setup identity carried across restart inspection of one ready generation. */
struct ready_minimizer_generation_identity {
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t storage_contract_checksum = 0;

    /** Compares every authenticated generation identity field. */
    [[nodiscard]] bool operator==(const ready_minimizer_generation_identity&) const = default;
};

/** Inspects the authenticated identity of one ready generation. */
[[nodiscard]] ready_minimizer_generation_identity inspect_ready_minimizer_generation_identity_pg(
    const minimizer_generation_administration_identity& identity,
    std::uint64_t sequence_rowno,
    Snapshot executor_snapshot,
    expected_minimizer_generation_identity expected);

/// Authenticated head, manifest, and directory returned by metadata discovery.
struct minimizer_generation_read_metadata {
    minimizer_generation_head head{};
    minimizer_generation_manifest manifest{};
    grouped_soa_directory_view directory{};
};

/// One caller-declared admission group. A group is one exact ordered PG range
/// scan. Every request must match one complete directory control/position
/// extent, and every skipped row must be predeclared by that directory.
struct minimizer_generation_payload_read_group {
    index_sub_key_range range{};
    std::vector<index_sub_key_request> requests;
};

namespace minimizer_generation_full_crc_detail {

/// Verify that incremental CRC concatenation matches a single byte stream.
[[nodiscard]] inline bool concatenation_self_test() noexcept {
    return bioseq::indexing::crc64_ecma182_concatenate(
               bioseq::indexing::crc64_ecma182(std::string_view{"control"}),
               bioseq::indexing::crc64_ecma182(std::string_view{"position"}),
               std::string_view{"position"}.size()) ==
           bioseq::indexing::crc64_ecma182(std::string_view{"controlposition"});
}

} // namespace minimizer_generation_full_crc_detail

/// Select whether a read session owns a registered snapshot reference.
enum class minimizer_generation_snapshot_registration : std::uint8_t {
    register_copy,
    borrow_registered,
};

/// Current-generation target session. It binds one fresh heap OID, its exact
/// five-key primary index, one executor-owned data snapshot, and one query key
/// for the complete head -> metadata -> controls -> positions -> finish
/// lifecycle. Both payload sections retain the same open relations and snapshot.
class index_storage_fragment_generation_read_session {
  private:
    enum class operation_state : std::uint8_t {
        opened,
        head_read,
        metadata_pending_source,
        metadata_ready,
        controls_planned,
        controls_complete,
        positions_planned,
        payload_complete,
        full_generation_payload_complete,
        finished,
        failed,
        moved,
    };

    enum class payload_section : std::uint8_t {
        controls,
        positions,
    };

    static constexpr std::uint64_t k_ordered_range_scan_penalty_rows = 8u;

    using loaded_row_type = minimizer_generation_loaded_row;

    struct planned_group {
        index_sub_key_range range{};
        std::vector<index_sub_key_request> requests;
        std::size_t first_directory_ordinal = 0;
        std::size_t end_directory_ordinal = 0;
        std::uint64_t skip_budget_rows = 0;
        bool position_extent = false;
        bool executed = false;
    };

    struct runtime_request {
        index_sub_key_request request{};
        std::uint64_t end_chunk_no = 0;
        std::uint64_t next_chunk_no = 0;
        std::uint64_t total_bytes = 0;
        crc64_ecma182_accumulator checksum{};
    };

    std::string table_name_;
    Relation heap_relation_ = nullptr;
    Relation primary_index_relation_ = nullptr;
    Relation storage_descriptor_relation_ = nullptr;
    Relation storage_descriptor_live_index_relation_ = nullptr;
    SysScanDesc active_scan_ = nullptr;
    std::array<TupleTableSlot*, k_minimizer_generation_full_scan_pinned_batch_rows>
        full_generation_batch_slots_{};
    std::size_t full_generation_batch_slot_count_ = 0;
    Snapshot snapshot_ = nullptr;
    bool snapshot_registered_by_session_ = false;
    std::uint64_t query_key_ = 0;
    Oid expected_tablespace_oid_ = InvalidOid;
    expected_minimizer_generation_identity expected_{};
    grouped_soa_minimizer_profile expected_profile_{};
    operation_state state_ = operation_state::opened;
    bool catalog_source_authorized_ = false;
    // Permits precede every allocation they cover, so reverse member
    // destruction cannot return admitted bytes while an owned view/buffer is
    // still alive.
    bioseq::parallelism::memory_permit metadata_permit_;
    bioseq::parallelism::memory_permit plan_permit_;
    bioseq::parallelism::memory_permit runtime_permit_;
    bioseq::parallelism::memory_permit control_authorization_permit_;
    std::optional<minimizer_generation_head> head_;
    std::optional<minimizer_generation_read_metadata> metadata_;
    std::optional<minimizer_generation_manifest> manifest_only_;
    std::vector<std::byte> directory_bytes_scratch_;
    std::vector<planned_group> planned_groups_;
    std::vector<runtime_request> runtime_requests_;
    std::vector<std::uint32_t> authorized_position_ordinals_;
    std::size_t next_group_ = 0;
    std::optional<std::uint64_t> previous_payload_heap_block_;
    std::optional<std::uint64_t> payload_stage_begin_ns_;
    minimizer_generation_read_stats stats_{};

    /// Return the sole authenticated manifest authority. Full metadata and
    /// manifest-only state are mutually exclusive. Observing both or neither
    /// fails closed at every transition that consumes a manifest.
    [[nodiscard]] const minimizer_generation_manifest* active_manifest_() const noexcept;

    [[noreturn]] void fail_(std::string_view message);

    void checked_add_(std::uint64_t& value, std::uint64_t increment, std::string_view message);

    /// Classify one physical payload TID transition. Same-page and immediately
    /// next-page tuples are adjacent. Larger forward jumps and all backward
    /// jumps remain separately visible.
    void record_payload_tid_(ItemPointerData tid) noexcept;

    void record_payload_tid_(const HeapTuple tuple) noexcept;

    void start_payload_timing_() noexcept;

    /// Record one truthful coarse wall interval for the complete payload
    /// scan-and-callback stage. Decode and transfer remain explicitly
    /// unavailable (zero) until their owning sections expose coarse boundaries.
    /// no tuple/chunk-local clock reads are permitted on the cold path.
    void finalize_payload_timing_() noexcept;

    void release_owned_state_() noexcept;

    void release_section_plan_() noexcept;

    void release_control_authorization_() noexcept;

    void checked_add_read_stats_(index_chunk_read_stats& destination,
                                 const index_chunk_read_stats& increment);

    [[nodiscard]] static constexpr bool
    counter_is_sum_(std::uint64_t total, std::uint64_t left, std::uint64_t right) noexcept {
        return total >= left && total - left == right;
    }

    [[nodiscard]] static constexpr bool
    read_stats_are_sum_(const index_chunk_read_stats& total,
                        const index_chunk_read_stats& left,
                        const index_chunk_read_stats& right) noexcept {
        return counter_is_sum_(total.relation_opens, left.relation_opens, right.relation_opens) &&
               counter_is_sum_(total.index_scans, left.index_scans, right.index_scans) &&
               counter_is_sum_(
                   total.requested_ranges, left.requested_ranges, right.requested_ranges) &&
               counter_is_sum_(total.heap_rows_read, left.heap_rows_read, right.heap_rows_read) &&
               counter_is_sum_(
                   total.toast_values_read, left.toast_values_read, right.toast_values_read) &&
               counter_is_sum_(total.detoast_bytes, left.detoast_bytes, right.detoast_bytes) &&
               counter_is_sum_(total.explicit_copy_bytes,
                               left.explicit_copy_bytes,
                               right.explicit_copy_bytes) &&
               counter_is_sum_(total.wanted_subkeys, left.wanted_subkeys, right.wanted_subkeys) &&
               counter_is_sum_(total.read_subkeys, left.read_subkeys, right.read_subkeys) &&
               counter_is_sum_(total.wanted_extents, left.wanted_extents, right.wanted_extents) &&
               counter_is_sum_(total.read_extents, left.read_extents, right.read_extents) &&
               counter_is_sum_(total.wanted_chunks, left.wanted_chunks, right.wanted_chunks) &&
               counter_is_sum_(total.skipped_rows, left.skipped_rows, right.skipped_rows) &&
               counter_is_sum_(
                   total.skip_budget_rows, left.skip_budget_rows, right.skip_budget_rows) &&
               counter_is_sum_(total.repeated_chunk_reads,
                               left.repeated_chunk_reads,
                               right.repeated_chunk_reads) &&
               counter_is_sum_(
                   total.delivered_chunks, left.delivered_chunks, right.delivered_chunks) &&
               counter_is_sum_(total.delivered_bytes, left.delivered_bytes, right.delivered_bytes);
    }

    [[nodiscard]] static constexpr bool
    read_stats_are_zero_(const index_chunk_read_stats& stats) noexcept {
        return read_stats_are_sum_(stats, index_chunk_read_stats{}, index_chunk_read_stats{});
    }

    [[nodiscard]] static constexpr bool
    section_stats_are_zero_(const minimizer_generation_payload_stage_read_stats& section) noexcept {
        return section.plan_calls == 0u && section.planned_request_groups == 0u &&
               section.executed_request_groups == 0u && section.range_scan_calls == 0u &&
               section.physical_heap_rows_read == 0u && section.physical_bytes_read == 0u &&
               section.control_authorization_checks == 0u && read_stats_are_zero_(section.read);
    }

    [[nodiscard]] static constexpr bool full_payload_stats_are_zero_(
        const minimizer_generation_full_payload_read_stats& full) noexcept {
        return full.scan_calls == 0u && full.planned_extents == 0u &&
               full.completed_extents == 0u && full.control_chunks == 0u &&
               full.position_chunks == 0u && full.control_bytes == 0u &&
               full.position_bytes == 0u && full.payload_row_checksum == 0u;
    }

    [[nodiscard]] static constexpr bool
    section_equations_hold_(const minimizer_generation_payload_stage_read_stats& section) noexcept {
        return section.plan_calls == 1 &&
               section.planned_request_groups == section.executed_request_groups &&
               section.executed_request_groups == section.range_scan_calls &&
               section.read.relation_opens == 0 &&
               section.read.index_scans == section.range_scan_calls &&
               section.read.requested_ranges == section.range_scan_calls &&
               section.read.heap_rows_read == section.physical_heap_rows_read &&
               section.read.heap_rows_read ==
                   section.read.delivered_chunks + section.read.skipped_rows &&
               section.read.toast_values_read == 0 && section.read.detoast_bytes == 0 &&
               section.read.explicit_copy_bytes == 0 &&
               section.read.wanted_subkeys == section.read.read_subkeys &&
               section.read.wanted_extents == section.read.read_extents &&
               section.read.wanted_chunks == section.read.delivered_chunks &&
               section.read.skipped_rows == section.read.skip_budget_rows &&
               section.read.repeated_chunk_reads == 0 &&
               section.physical_bytes_read >= section.read.delivered_bytes;
    }

    /// Allocate every auxiliary BufferHeapTuple slot before the batch reader
    /// starts. The count advances after each successful allocation so an ERROR
    /// part-way through setup leaves an exact cleanup prefix.
    void create_full_generation_batch_slots_checked_();

    /// Drop pins and executor slots explicitly while PostgreSQL cleanup is
    /// still legal. Retire each pointer before its PG call so ResourceOwner is
    /// the single fallback if that call raises ERROR, with no later double-drop.
    void drop_full_generation_batch_slots_checked_();

    void abandon_relations_noexcept_() noexcept;

    void close_relations_checked_();

    /// Preserve the original PG/C++ failure while deterministically ending an
    /// active scan and releasing both relation locks plus snapshot ownership.
    /// Cleanup runs after invoke_pg_as_cpp restored PostgreSQL's exception
    /// stack, never from a destructor during C++ unwinding.
    [[noreturn]] void cleanup_failed_session_and_rethrow_();

    struct projected_extent_range {
        index_sub_key_t logical_begin = 0;
        index_sub_key_t logical_end = 0;
        bool position_extent = false;
    };

    /// Project one physical range onto the logical/control directory key
    /// namespace. A range may cover controls or positions, never cross the
    /// namespace boundary.
    [[nodiscard]] static std::optional<projected_extent_range>
    project_extent_range_(const index_sub_key_range& range) noexcept;

    [[nodiscard]] static bool
    request_matches_extent_(const index_sub_key_request& request,
                            const grouped_soa_bucket_directory_entry& entry,
                            bool position_extent) noexcept;

  public:
    index_storage_fragment_generation_read_session() = delete;
    /// Bind one query and expected generation identity to a registered MVCC
    /// snapshot without opening payload scans.
    index_storage_fragment_generation_read_session(
        std::string_view table_name,
        std::uint64_t query_key,
        Snapshot executor_snapshot,
        expected_minimizer_generation_identity expected,
        Oid expected_tablespace_oid,
        minimizer_generation_snapshot_registration snapshot_registration =
            minimizer_generation_snapshot_registration::register_copy,
        std::string_view classification_index_name = {});
    index_storage_fragment_generation_read_session(
        const index_storage_fragment_generation_read_session&) = delete;
    index_storage_fragment_generation_read_session&
    operator=(const index_storage_fragment_generation_read_session&) = delete;
    index_storage_fragment_generation_read_session(
        index_storage_fragment_generation_read_session&& other) noexcept;
    index_storage_fragment_generation_read_session&
    operator=(index_storage_fragment_generation_read_session&&) = delete;
    ~index_storage_fragment_generation_read_session();

  private:
    [[nodiscard]] minimizer_generation_head read_active_head_impl_(bool bind_expected_layout);

  public:
    /// Read and authenticate an active head bound to the caller's independently
    /// selected expected format.
    [[nodiscard]] minimizer_generation_head read_active_head_once();

    /// Administrative wire-format discovery. This authenticates the head,
    /// defers only its expected-format comparison, and closes the head-only
    /// session before returning. The caller must reopen a normally bound
    /// session before reading manifest or payload bytes.
    [[nodiscard]] minimizer_generation_head discover_active_head_layout_and_close();

    /// Timed compact-direct metadata path. Read the exact manifest tuple under
    /// the same snapshot as the head, using chunk_no=0 in the primary-index
    /// qualification so no directory tuple can be visited. The fixed manifest
    /// and its head checksum remain fully authenticated. Only the O(directory)
    /// materialization/CRC/grouped_soa_directory_view construction is intentionally omitted.
    [[nodiscard]] const minimizer_generation_manifest&
    read_manifest_once_without_directory(const minimizer_generation_head& expected_head);

    /// Read and authenticate the immutable manifest and grouped directory once.
    [[nodiscard]] const minimizer_generation_read_metadata&
    read_manifest_directory_once(const minimizer_generation_head& expected_head);

    /// Bind metadata to an independently reopened source row. This is the only
    /// transition to metadata_ready: source identity alone is insufficient.
    void
    consume_full_source_authorization(minimizer_generation_source_authorization&& authorization);

    /// Timed cold-reader transition. Only the constant-work catalog authorization is
    /// accepted here. The full decoded authorization has a separate, explicitly named
    /// untimed writer/admin transition above.
    void
    consume_source_authorization(minimizer_generation_catalog_source_authorization&& authorization);

    /// Bind the exact LIVE image descriptor under the same registered MVCC
    /// snapshot that supplied the head and manifest.  Both catalog locks stay
    /// owned by this session until the mapped image has been registered and
    /// the selected mapped grouped-SoA finish transition performs the checked
    /// close.
    [[nodiscard]] minimizer_generation_storage_descriptor
    read_live_storage_descriptor_once(const minimizer_generation_administration_identity& identity);

  private:
    [[nodiscard]] minimizer_generation_read_stats finish_full_generation_();

    void plan_section_groups_(payload_section section,
                              std::span<const minimizer_generation_payload_read_group> groups);

  public:
    /// Stream the complete authorized generation through one ordered physical
    /// scan. Payload bytes remain opaque and callback-scoped: this layer does
    /// not decode controls, globalize/rebase local positions, or retain a
    /// payload image.  A callback may copy into already-admitted host or mapped
    /// device storage, but command submission/fence waits must occur only after
    /// this method returns and `active_scan_` has been closed.
    template <class p_control_sink, class p_position_sink>
        requires(bioseq::indexing::index_chunk_reader_fn<p_control_sink> ||
                 std::invocable<p_control_sink&,
                                const validated_minimizer_generation_row_receipt&>) &&
                (bioseq::indexing::index_chunk_reader_fn<p_position_sink> ||
                 std::invocable<p_position_sink&,
                                const validated_minimizer_generation_row_receipt&>)
    [[nodiscard]] index_chunk_read_stats
    read_full_generation_payload_once(p_control_sink&& control_sink,
                                      p_position_sink&& position_sink) {
        index_chunk_read_stats local{};
        const BufferUsage payload_buffer_begin = pgBufferUsage;
        start_payload_timing_();
        try {
            if (!minimizer_generation_full_crc_detail::concatenation_self_test()) {
                fail_("full-generation CRC concatenation self-test failed");
            }
            if (state_ != operation_state::metadata_ready || !metadata_ ||
                !catalog_source_authorized_ || active_scan_ != nullptr ||
                full_generation_batch_slot_count_ != 0 || !planned_groups_.empty() ||
                next_group_ != 0 || !runtime_requests_.empty() || plan_permit_ || runtime_permit_ ||
                control_authorization_permit_ || !authorized_position_ordinals_.empty()) {
                checked_add_(stats_.payload.repeated_chunk_reads,
                             1,
                             "full-generation repeated-read counter overflow");
                fail_("full-generation payload scan was repeated, early, lacked catalog "
                      "authorization, or found selected-range state");
            }
            const auto entries = metadata_->directory.entries();
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(metadata_->manifest.layout);
            if (extent_payload_bytes == 0) {
                fail_("full-generation wire quantum is invalid");
            }
            if (entries.size() > (std::numeric_limits<std::uint64_t>::max)() / 2u) {
                fail_("full-generation extent count overflow");
            }
            const std::uint64_t planned_extents = static_cast<std::uint64_t>(entries.size()) * 2u;
            if (planned_extents != metadata_->manifest.extent_count) {
                fail_("full-generation directory extent equation disagrees");
            }

            // The primary key yields every global control sub-key followed by
            // every derived position sub-key. Grouped-SoA hashes that same
            // physical order, so the payload checksum can be streamed directly.
            std::uint64_t total_chunks = 0;
            std::uint64_t planned_control_chunks = 0;
            std::uint64_t planned_position_chunks = 0;
            std::uint64_t planned_control_bytes = 0;
            std::uint64_t planned_position_bytes = 0;
            for (const grouped_soa_bucket_directory_entry& entry : entries) {
                checked_add_(planned_control_chunks,
                             entry.control_chunk_count,
                             "full-generation control chunk count overflow");
                checked_add_(planned_position_chunks,
                             entry.position_chunk_count,
                             "full-generation position chunk count overflow");
                checked_add_(planned_control_bytes,
                             entry.control_encoded_bytes,
                             "full-generation control byte count overflow");
                checked_add_(planned_position_bytes,
                             entry.position_encoded_bytes,
                             "full-generation position byte count overflow");
            }
            checked_add_(
                total_chunks, planned_control_chunks, "full-generation total chunk count overflow");
            checked_add_(total_chunks,
                         planned_position_chunks,
                         "full-generation total chunk count overflow");
            local.index_scans = 1;
            local.requested_ranges = 1;
            local.wanted_subkeys = planned_extents;
            local.wanted_extents = planned_extents;
            local.wanted_chunks = total_chunks;
            stats_.cold.route_full = 1;
            stats_.cold.add(stats_.cold.payload_rows_requested, total_chunks);
            stats_.cold.add(stats_.cold.payload_bytes_requested, metadata_->manifest.payload_bytes);

            ScanKeyData keys[3];
            ScanKeyInit(
                &keys[0],
                k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber,
                F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(metadata_->manifest.generation_id)));
            ScanKeyInit(&keys[1],
                        k_minimizer_generation_index_query_key,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2],
                        k_minimizer_generation_index_position_segment,
                        BTGreaterEqualStrategyNumber,
                        F_INT4GE,
                        Int32GetDatum(0));
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &keys] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 3, keys);
            });
            create_full_generation_batch_slots_checked_();
            stats_.cold.add(stats_.cold.payload_range_scans, 1);

            std::size_t entry_index = 0;
            std::uint32_t extent_chunk = 0;
            bool position_extent = false;
            bool payload_complete = entries.empty();
            crc64_ecma182_accumulator extent_checksum{};
            crc64_ecma182_accumulator payload_row_checksum{};

            // One bridge covers up to K fetch/deform/projection operations.
            // Each projection borrows its own pinned slot; row authority, CRC
            // and arbitrary sinks execute only after PG_END_TRY, so neither a
            // receipt nor sink-owned C++ state crosses a siglongjmp boundary.
            pinned_minimizer_generation_projected_row_batch batch{};
            std::size_t batch_ordinal = 0;
            for (;;) {
                if (batch_ordinal == batch.size) {
                    if (batch.eof)
                        break;
                    batch = next_pinned_minimizer_generation_projected_row_batch_or_error(
                        heap_relation_, active_scan_, full_generation_batch_slots_);
                    batch_ordinal = 0;
                    if (batch.size == 0) {
                        if (batch.eof)
                            break;
                        fail_("full-generation batched payload scan made no progress");
                    }
                }
                const pinned_minimizer_generation_projected_row& pinned =
                    batch.rows[batch_ordinal++];
                record_payload_tid_(pinned.tid);
                loaded_row_type row =
                    load_minimizer_generation_projected_row_or_error(pinned.projected);
                stats_.cold.add(stats_.cold.row_crc_bytes, row.bytes.size());
                stats_.cold.add(stats_.cold.extent_crc_bytes, row.bytes.size());
                stats_.cold.add(stats_.cold.payload_rows_scanned, 1);
                stats_.cold.add(stats_.cold.payload_bytes_scanned, row.bytes.size());
                checked_add_(local.heap_rows_read, 1, "full-generation heap row count overflow");
                if (payload_complete || entry_index >= entries.size()) {
                    row.release();
                    fail_("full-generation payload scan returned extra rows");
                }
                const grouped_soa_bucket_directory_entry& entry = entries[entry_index];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    row.release();
                    fail_("full-generation position sub-key is invalid");
                }
                const std::uint32_t expected_chunk_count =
                    position_extent ? entry.position_chunk_count : entry.control_chunk_count;
                const std::uint32_t first_chunk =
                    position_extent ? entry.position_first_chunk : entry.control_first_chunk;
                const std::uint64_t expected_extent_bytes =
                    position_extent ? entry.position_encoded_bytes : entry.control_encoded_bytes;
                if (extent_chunk >= expected_chunk_count ||
                    extent_chunk >
                        (std::numeric_limits<std::uint64_t>::max)() / extent_payload_bytes) {
                    row.release();
                    fail_("full-generation extent chunk cursor overflow");
                }
                const std::uint64_t extent_offset =
                    static_cast<std::uint64_t>(extent_chunk) * extent_payload_bytes;
                if (extent_offset >= expected_extent_bytes) {
                    row.release();
                    fail_("full-generation extent byte cursor overflow");
                }
                const std::uint64_t expected_bytes = std::min<std::uint64_t>(
                    extent_payload_bytes, expected_extent_bytes - extent_offset);
                const index_chunk_address expected_address{
                    .generation_id = metadata_->manifest.generation_id,
                    .position_segment = entry.position_segment,
                    .sub_key = position_extent ? *position_sub_key : entry.sub_key,
                    .chunk_no = static_cast<std::uint32_t>(first_chunk + extent_chunk),
                };
                if (row.query_key != query_key_ || row.address != expected_address ||
                    row.bytes.size() != expected_bytes) {
                    row.release();
                    fail_(
                        "full-generation payload rows are missing, extra, duplicate, or reordered");
                }

                std::uint64_t validated_row_checksum = 0u;
                bool receipt_matches_row = false;
                const bool receipt_visited =
                    row.validated_receipt.has_value() &&
                    bioseq::indexing::visit_validated_minimizer_generation_row(
                        *row.validated_receipt,
                        [&row, &receipt_matches_row, &validated_row_checksum](
                            const index_chunk_address& receipt_address,
                            std::span<const std::byte> receipt_bytes,
                            std::uint64_t receipt_checksum) noexcept {
                            receipt_matches_row = receipt_address == row.address &&
                                                  receipt_bytes.data() == row.bytes.data() &&
                                                  receipt_bytes.size() == row.bytes.size();
                            validated_row_checksum = receipt_checksum;
                        });
                if (!receipt_visited || !receipt_matches_row) {
                    row.release();
                    fail_("full-generation row receipt lost validated authority");
                }

                extent_checksum.value =
                    bioseq::indexing::crc64_append_validated_index_chunk_payload(
                        extent_checksum.checksum(),
                        row.address,
                        row.chunk_bytes,
                        validated_row_checksum);
                stats_.cold.add(stats_.cold.extent_crc_combined_bytes, row.bytes.size());
                stats_.cold.add(stats_.cold.extent_crc_combine_rows, 1);
                payload_row_checksum.update_little_endian(validated_row_checksum);
                if (position_extent) {
                    if constexpr (std::invocable<
                                      p_position_sink&,
                                      const validated_minimizer_generation_row_receipt&>) {
                        std::invoke(position_sink, *row.validated_receipt);
                    } else {
                        std::invoke(position_sink,
                                    row.address,
                                    reinterpret_cast<const char*>(row.bytes.data()),
                                    row.bytes.size());
                    }
                    checked_add_(stats_.full_generation.position_chunks,
                                 1,
                                 "full-generation position chunk counter overflow");
                    checked_add_(stats_.full_generation.position_bytes,
                                 row.bytes.size(),
                                 "full-generation position byte counter overflow");
                } else {
                    if constexpr (std::invocable<
                                      p_control_sink&,
                                      const validated_minimizer_generation_row_receipt&>) {
                        std::invoke(control_sink, *row.validated_receipt);
                    } else {
                        std::invoke(control_sink,
                                    row.address,
                                    reinterpret_cast<const char*>(row.bytes.data()),
                                    row.bytes.size());
                    }
                    checked_add_(stats_.full_generation.control_chunks,
                                 1,
                                 "full-generation control chunk counter overflow");
                    checked_add_(stats_.full_generation.control_bytes,
                                 row.bytes.size(),
                                 "full-generation control byte counter overflow");
                }
                checked_add_(
                    local.delivered_chunks, 1, "full-generation delivered chunk counter overflow");
                checked_add_(local.delivered_bytes,
                             row.bytes.size(),
                             "full-generation delivered byte counter overflow");
                stats_.cold.add(stats_.cold.payload_rows_delivered, 1);
                stats_.cold.add(stats_.cold.payload_bytes_delivered, row.bytes.size());
                row.release();

                ++extent_chunk;
                if (extent_chunk == expected_chunk_count) {
                    const std::uint64_t expected_extent_checksum =
                        position_extent ? entry.position_checksum : entry.control_checksum;
                    if (extent_checksum.checksum() != expected_extent_checksum) {
                        fail_("full-generation extent checksum disagrees");
                    }
                    checked_add_(stats_.full_generation.completed_extents,
                                 1,
                                 "full-generation completed extent counter overflow");
                    extent_chunk = 0;
                    extent_checksum = {};
                    ++entry_index;
                    if (entry_index == entries.size()) {
                        if (!position_extent) {
                            position_extent = true;
                            entry_index = 0;
                        } else {
                            payload_complete = true;
                        }
                    }
                }
            }
            drop_full_generation_batch_slots_checked_();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;
            if (!payload_complete || extent_chunk != 0 ||
                stats_.full_generation.completed_extents != planned_extents ||
                stats_.full_generation.control_chunks != planned_control_chunks ||
                stats_.full_generation.position_chunks != planned_position_chunks ||
                stats_.full_generation.control_bytes != planned_control_bytes ||
                stats_.full_generation.position_bytes != planned_position_bytes) {
                fail_("full-generation payload scan ended before its complete directory plan");
            }
            stats_.full_generation.payload_row_checksum = payload_row_checksum.checksum();
            if (stats_.full_generation.payload_row_checksum !=
                metadata_->manifest.payload_checksum) {
                fail_("full-generation manifest payload checksum disagrees");
            }
            local.read_subkeys = local.wanted_subkeys;
            local.read_extents = local.wanted_extents;
            if (local.heap_rows_read != local.delivered_chunks ||
                local.delivered_chunks != local.wanted_chunks ||
                local.delivered_bytes != metadata_->manifest.payload_bytes) {
                fail_("full-generation payload authorization equations disagree");
            }
            stats_.full_generation.scan_calls = 1;
            stats_.full_generation.planned_extents = planned_extents;
            stats_.planned_request_groups = 1;
            stats_.executed_request_groups = 1;
            stats_.bulk_read_calls = 1;
            stats_.range_scan_calls = 1;
            checked_add_(stats_.index_scans, 1, "full-generation index scan counter overflow");
            checked_add_read_stats_(stats_.payload, local);
            stats_.payload_heap_rows_read = local.heap_rows_read;
            stats_.payload_bytes_read = local.delivered_bytes;
            add_cold_index_buffer_usage(stats_.cold, payload_buffer_begin);
            state_ = operation_state::full_generation_payload_complete;
        } catch (...) {
            cleanup_failed_session_and_rethrow_();
        }
        return local;
    }

    void plan_control_groups(std::span<const minimizer_generation_payload_read_group> groups);

    void plan_position_groups(std::span<const minimizer_generation_payload_read_group> groups);

    template <class p_sink>
        requires bioseq::indexing::index_chunk_reader_fn<p_sink> ||
                 std::invocable<p_sink&, const validated_minimizer_generation_row_receipt&>
    [[nodiscard]] index_chunk_read_stats read_chunks_in_range(std::size_t group_index,
                                                              index_chunk_cache_policy cache_policy,
                                                              p_sink&& sink) {
        index_chunk_read_stats local{};
        const BufferUsage payload_buffer_begin = pgBufferUsage;
        start_payload_timing_();
        try {
            const bool position_section = state_ == operation_state::positions_planned;
            if ((state_ != operation_state::controls_planned && !position_section) ||
                group_index != next_group_ || group_index >= planned_groups_.size()) {
                checked_add_(stats_.payload.repeated_chunk_reads,
                             1,
                             "payload repeated-read counter overflow");
                fail_("payload group was repeated, reordered, or not planned");
            }
            if (cache_policy != index_chunk_cache_policy::bypass_one_shot) {
                fail_("authorized generation session requires bypass_one_shot");
            }
            planned_group& group = planned_groups_[group_index];
            if (group.position_extent != position_section || group.executed ||
                active_scan_ != nullptr || !runtime_requests_.empty() || runtime_permit_) {
                checked_add_(stats_.payload.repeated_chunk_reads,
                             1,
                             "payload repeated-read counter overflow");
                fail_("payload group was executed more than once or session state leaked");
            }
            if (group.requests.size() >
                (std::numeric_limits<std::uint64_t>::max)() / sizeof(runtime_request)) {
                fail_("payload runtime admission size overflow");
            }
            const std::uint64_t runtime_bytes = group.requests.size() * sizeof(runtime_request);
            auto runtime_permit =
                bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                    runtime_bytes);
            if (!runtime_permit) {
                fail_("payload runtime memory admission was denied");
            }
            runtime_permit_ = std::move(*runtime_permit);
            runtime_requests_.clear();
            runtime_requests_.reserve(group.requests.size());
            local.index_scans = 1;
            local.requested_ranges = 1;
            local.wanted_extents = group.requests.size();
            local.skip_budget_rows = group.skip_budget_rows;
            std::optional<index_sub_key_t> previous_subkey;
            for (const index_sub_key_request& request : group.requests) {
                runtime_requests_.push_back(runtime_request{
                    .request = request,
                    .end_chunk_no = static_cast<std::uint64_t>(request.first_chunk_no) +
                                    request.expected_chunk_count,
                    .next_chunk_no = request.first_chunk_no,
                });
                if (!previous_subkey || *previous_subkey != request.sub_key) {
                    checked_add_(local.wanted_subkeys, 1, "payload wanted sub-key count overflow");
                    previous_subkey = request.sub_key;
                }
                checked_add_(local.wanted_chunks,
                             request.expected_chunk_count,
                             "payload local wanted chunk count overflow");
            }

            ScanKeyData keys[5];
            ScanKeyInit(&keys[0],
                        k_minimizer_generation_index_generation_id,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        Int64GetDatum(static_cast<std::int64_t>(group.range.generation_id)));
            ScanKeyInit(&keys[1],
                        k_minimizer_generation_index_query_key,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&keys[2],
                        k_minimizer_generation_index_sub_key,
                        BTGreaterEqualStrategyNumber,
                        F_INT8GE,
                        Int64GetDatum(static_cast<std::int64_t>(group.range.begin)));
            ScanKeyInit(&keys[3],
                        k_minimizer_generation_index_sub_key,
                        BTLessStrategyNumber,
                        F_INT8LT,
                        Int64GetDatum(static_cast<std::int64_t>(group.range.end)));
            ScanKeyInit(&keys[4],
                        k_minimizer_generation_index_position_segment,
                        BTEqualStrategyNumber,
                        F_INT4EQ,
                        Int32GetDatum(group.range.position_segment));
            active_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &keys] {
                return systable_beginscan_ordered(
                    heap_relation_, primary_index_relation_, snapshot_, 5, keys);
            });
            stats_.cold.add(stats_.cold.payload_range_scans, 1);

            const auto entries = metadata_->directory.entries();
            std::size_t directory_ordinal = group.first_directory_ordinal;
            std::uint64_t expected_chunk_no = 0;
            std::size_t request_cursor = 0;
            std::uint64_t physical_payload_bytes = 0;
            for (HeapTuple tuple;
                 (tuple = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                      return systable_getnext_ordered(active_scan_, ForwardScanDirection);
                  })) != nullptr;) {
                record_payload_tid_(tuple);
                loaded_row_type row = load_minimizer_generation_row_or_error(heap_relation_, tuple);
                stats_.cold.add(stats_.cold.row_crc_bytes, row.bytes.size());
                stats_.cold.add(stats_.cold.payload_rows_scanned, 1);
                stats_.cold.add(stats_.cold.payload_bytes_scanned, row.bytes.size());
                checked_add_(local.heap_rows_read, 1, "payload heap row count overflow");
                checked_add_(physical_payload_bytes,
                             row.bytes.size(),
                             "payload physical byte count overflow");
                if (row.query_key != query_key_ ||
                    row.address.generation_id != group.range.generation_id ||
                    row.address.position_segment != group.range.position_segment ||
                    row.address.sub_key < group.range.begin ||
                    row.address.sub_key >= group.range.end) {
                    row.release();
                    fail_("payload scan returned a row outside its qualified range");
                }

                if (directory_ordinal >= group.end_directory_ordinal) {
                    row.release();
                    fail_("payload scan returned more rows than its directory range");
                }
                const grouped_soa_bucket_directory_entry& entry = entries[directory_ordinal];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    row.release();
                    fail_("payload directory position sub-key is invalid");
                }
                const index_sub_key_t expected_sub_key =
                    group.position_extent ? *position_sub_key : entry.sub_key;
                const std::uint64_t entry_chunk_count =
                    group.position_extent ? entry.position_chunk_count : entry.control_chunk_count;
                if (row.address.sub_key != expected_sub_key ||
                    row.address.chunk_no != expected_chunk_no ||
                    expected_chunk_no >= entry_chunk_count) {
                    row.release();
                    fail_("payload physical rows do not exactly equal directory order");
                }

                while (request_cursor < runtime_requests_.size()) {
                    runtime_request& request = runtime_requests_[request_cursor];
                    const bool row_after_request =
                        row.address.sub_key > request.request.sub_key ||
                        (row.address.sub_key == request.request.sub_key &&
                         static_cast<std::uint64_t>(row.address.chunk_no) >= request.end_chunk_no);
                    if (!row_after_request) {
                        break;
                    }
                    if (request.next_chunk_no != request.end_chunk_no ||
                        request.total_bytes != request.request.expected_total_bytes ||
                        request.checksum.checksum() != request.request.expected_checksum) {
                        row.release();
                        fail_("payload extent aggregate count/bytes/checksum disagrees");
                    }
                    ++request_cursor;
                }

                runtime_request* matched = nullptr;
                if (request_cursor < runtime_requests_.size()) {
                    runtime_request& request = runtime_requests_[request_cursor];
                    if (row.address.sub_key == request.request.sub_key &&
                        row.address.chunk_no >= request.request.first_chunk_no &&
                        static_cast<std::uint64_t>(row.address.chunk_no) < request.end_chunk_no) {
                        matched = &request;
                    }
                }
                if (matched == nullptr) {
                    checked_add_(local.skipped_rows, 1, "payload skipped row count overflow");
                    stats_.cold.add(stats_.cold.payload_rows_skipped, 1);
                } else {
                    if (matched->next_chunk_no != row.address.chunk_no) {
                        row.release();
                        fail_("payload extent is missing, duplicate, or non-contiguous");
                    }
                    checked_add_(matched->total_bytes,
                                 row.bytes.size(),
                                 "payload extent byte count overflow");
                    std::uint64_t validated_row_checksum = 0u;
                    bool receipt_matches_row = false;
                    const bool receipt_visited =
                        row.validated_receipt.has_value() &&
                        bioseq::indexing::visit_validated_minimizer_generation_row(
                            *row.validated_receipt,
                            [&row, &receipt_matches_row, &validated_row_checksum](
                                const index_chunk_address& receipt_address,
                                std::span<const std::byte> receipt_bytes,
                                std::uint64_t receipt_checksum) noexcept {
                                receipt_matches_row = receipt_address == row.address &&
                                                      receipt_bytes.data() == row.bytes.data() &&
                                                      receipt_bytes.size() == row.bytes.size();
                                validated_row_checksum = receipt_checksum;
                            });
                    if (!receipt_visited || !receipt_matches_row) {
                        row.release();
                        fail_("payload row receipt lost validated authority");
                    }
                    matched->checksum.value =
                        bioseq::indexing::crc64_append_validated_index_chunk_payload(
                            matched->checksum.checksum(),
                            row.address,
                            row.chunk_bytes,
                            validated_row_checksum);
                    stats_.cold.add(stats_.cold.extent_crc_bytes, row.bytes.size());
                    stats_.cold.add(stats_.cold.extent_crc_combined_bytes, row.bytes.size());
                    stats_.cold.add(stats_.cold.extent_crc_combine_rows, 1);
                    if constexpr (std::invocable<
                                      p_sink&,
                                      const validated_minimizer_generation_row_receipt&>) {
                        std::invoke(sink, *row.validated_receipt);
                    } else {
                        std::invoke(sink,
                                    row.address,
                                    reinterpret_cast<const char*>(row.bytes.data()),
                                    row.bytes.size());
                    }
                    ++matched->next_chunk_no;
                    checked_add_(
                        local.delivered_chunks, 1, "payload delivered chunk count overflow");
                    checked_add_(local.delivered_bytes,
                                 row.bytes.size(),
                                 "payload delivered byte count overflow");
                    stats_.cold.add(stats_.cold.payload_rows_delivered, 1);
                    stats_.cold.add(stats_.cold.payload_bytes_delivered, row.bytes.size());
                }
                row.release();
                ++expected_chunk_no;
                if (expected_chunk_no == entry_chunk_count) {
                    ++directory_ordinal;
                    expected_chunk_no = 0;
                }
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(active_scan_); });
            active_scan_ = nullptr;

            if (directory_ordinal != group.end_directory_ordinal || expected_chunk_no != 0) {
                fail_("payload scan ended before its complete directory range");
            }
            for (; request_cursor < runtime_requests_.size(); ++request_cursor) {
                const runtime_request& request = runtime_requests_[request_cursor];
                if (request.next_chunk_no != request.end_chunk_no ||
                    request.total_bytes != request.request.expected_total_bytes ||
                    request.checksum.checksum() != request.request.expected_checksum) {
                    fail_("payload extent aggregate count/bytes/checksum disagrees");
                }
            }
            local.read_subkeys = local.wanted_subkeys;
            local.read_extents = local.wanted_extents;
            if (local.delivered_chunks != local.wanted_chunks ||
                local.heap_rows_read != local.delivered_chunks + local.skipped_rows ||
                local.skipped_rows != local.skip_budget_rows) {
                fail_("payload authorization-counter equation disagrees");
            }

            group.executed = true;
            ++next_group_;
            const bool section_complete = next_group_ == planned_groups_.size();
            auto& section_stats = position_section ? stats_.positions : stats_.controls;
            checked_add_(section_stats.executed_request_groups,
                         1,
                         "payload section executed-group counter overflow");
            checked_add_(
                section_stats.range_scan_calls, 1, "payload section range-scan counter overflow");
            checked_add_(section_stats.physical_heap_rows_read,
                         local.heap_rows_read,
                         "payload section physical-row counter overflow");
            checked_add_(section_stats.physical_bytes_read,
                         physical_payload_bytes,
                         "payload section physical-byte counter overflow");
            checked_add_read_stats_(section_stats.read, local);
            checked_add_(
                stats_.executed_request_groups, 1, "executed request group count overflow");
            checked_add_(stats_.range_scan_calls, 1, "payload range scan count overflow");
            checked_add_(stats_.index_scans, 1, "total index scan count overflow");
            checked_add_read_stats_(stats_.payload, local);
            stats_.payload_heap_rows_read = stats_.payload.heap_rows_read;
            checked_add_(stats_.payload_bytes_read,
                         physical_payload_bytes,
                         "payload physical byte count overflow");
            std::vector<runtime_request>{}.swap(runtime_requests_);
            runtime_permit_.release();
            if (section_complete) {
                release_section_plan_();
                state_ = position_section ? operation_state::payload_complete
                                          : operation_state::controls_complete;
            }
            add_cold_index_buffer_usage(stats_.cold, payload_buffer_begin);
        } catch (const bioseq::postgres::support::postgres_error&) {
            state_ = operation_state::failed;
            abandon_relations_noexcept_();
            throw;
        } catch (...) {
            state_ = operation_state::failed;
            throw;
        }
        return local;
    }

    /// Checked success close for a publication verifier that intentionally
    /// rereads only the active head. Borrowed snapshots remain caller-owned.
    void close_head_read();

    /// Finish a grouped-SoA read whose immutable payload was consumed from the
    /// exact generation-bound mmap image. PostgreSQL remains the authority for
    /// the active head, manifest, directory and source authorization. The image
    /// itself was created only by the complete authenticated READY reread and
    /// was fsynced and durably renamed before publication.
    [[nodiscard]] minimizer_generation_read_stats finish_mapped_minimizer_fragment_payload();

    /// Checked success close for the timed compact-H32 mmap route. PostgreSQL
    /// authenticates exactly one head row and one manifest row. The exact
    /// chunk_no=0 scan qualification ensures that no directory tuple was read.
    /// Any control/payload planning or scan stats make this transition
    /// invalid rather than silently falling back to the relation reader.
    [[nodiscard]] minimizer_generation_read_stats finish_mapped_compact_direct();

    /// Checked success close for callers that intentionally validate only the
    /// immutable head/manifest/directory contract and do not enter payload
    /// sections. Borrowed snapshots remain caller-owned.
    void close_metadata_read();

    /// Validate stage equations, close every PG resource, and return the receipt.
    [[nodiscard]] minimizer_generation_read_stats finish();
};

/// Normal open outcome with either an owned session or a classified relation failure.
struct minimizer_generation_read_session_open_result {
    std::optional<index_storage_fragment_generation_read_session> session{};
    minimizer_generation_relation_inspection failure{
        .kind = minimizer_generation_relation_kind::malformed,
        .schema_error = minimizer_generation_schema_error::none,
    };

    [[nodiscard]] explicit operator bool() const noexcept {
        return session.has_value();
    }
};

/// Bind classification and reading to one heap descriptor and its primary
/// index. Unrelated secondary indexes are deliberately neither opened nor
/// counted.
/// A successful result owns both AccessShareLock references until finish(). An
/// unsuccessful normal result owns neither.  PostgreSQL ERROR remains an
/// exception so the transaction ResourceOwner retains its ordinary authority.
[[nodiscard]] inline minimizer_generation_read_session_open_result
open_exact_minimizer_generation_read_session_pg(std::string_view table_name,
                                                std::string_view index_name,
                                                std::uint64_t query_key,
                                                const borrowed_executor_read_context& executor_read,
                                                expected_minimizer_generation_identity expected) {
    if (!executor_read.valid()) {
        minimizer_generation_storage_error_as_cpp(
            "generation read requires a valid borrowed executor context");
    }
    minimizer_generation_read_session_open_result result{};
    try {
        result.session.emplace(table_name,
                               query_key,
                               executor_read.snapshot(),
                               expected,
                               executor_read.tablespace_oid(),
                               minimizer_generation_snapshot_registration::borrow_registered,
                               index_name);
    } catch (const minimizer_generation_relation_open_failure& failure) {
        result.failure = failure.inspection;
    }
    return result;
}

static_assert(!std::is_default_constructible_v<index_storage_fragment_generation_read_session>);
static_assert(!std::is_copy_constructible_v<index_storage_fragment_generation_read_session>);
static_assert(std::is_nothrow_move_constructible_v<index_storage_fragment_generation_read_session>);
static_assert(!std::is_move_assignable_v<index_storage_fragment_generation_read_session>);

} // namespace bioseq::storage::indexing
