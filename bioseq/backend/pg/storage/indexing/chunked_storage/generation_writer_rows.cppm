/**
 * @file generation_writer_rows.cppm
 * @brief Write generation rows in batches and reread them before publication.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sdir.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:generation_writer_rows;
import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::invoke_pg_as_cpp
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // index storage concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // minimizer fragment file format
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // decode sequence bases in batches
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
import :file_actions;
import :fragment_image;
export namespace bioseq::storage::indexing {
/// Write the rows of one generation and verify them before publication.
/// Buffer inserts within row and byte limits, then reread, the manifest,
/// directory  and payload against the encoder's checksums. A successful reread
/// produces the token required to publish the active head.
class index_storage_fragment_generation_row_sink {
  private:
    enum class write_stage : std::uint8_t {
        payload,
        directory,
        manifest,
        tokenized,
        failed,
        moved,
    };

    static constexpr std::uint64_t k_write_batch_rows = 4096;
    static constexpr std::uint64_t k_write_batch_bytes = MAX_CATALOG_MULTI_INSERT_BYTES;

    minimizer_generation_administration_identity identity_{};
    Relation relation_ = nullptr;
    CatalogIndexState catalog_indexes_ = nullptr;
    Relation validation_primary_index_ = nullptr;
    Snapshot validation_snapshot_ = nullptr;
    SysScanDesc validation_scan_ = nullptr;
    std::uint64_t advisory_lock_key_ = 0;
    std::uint64_t query_key_ = 0;
    std::uint64_t generation_id_ = 0;
    std::uint64_t row_count_ = 0;
    std::uint64_t write_batch_row_limit_ = k_write_batch_rows;
    std::uint64_t catalog_multi_insert_batches_ = 0;
    std::vector<TupleTableSlot*> pending_slots_;
    std::uint64_t rows_in_batch_ = 0;
    std::uint64_t bytes_in_batch_ = 0;
    std::uint64_t payload_rows_written_ = 0;
    std::uint64_t payload_bytes_written_ = 0;
    crc64_ecma182_accumulator payload_row_checksums_{};
    std::uint32_t next_directory_chunk_ = 1;
    std::uint64_t directory_bytes_written_ = 0;
    crc64_ecma182_accumulator directory_checksum_{};
    std::optional<index_chunk_address> previous_payload_address_;
    bool global_payload_order_possible_ = true;
    std::array<std::byte, minimizer_generation_manifest::encoded_bytes> written_manifest_bytes_{};
    std::uint32_t written_manifest_size_ = 0;
    std::optional<minimizer_generation_source_authorization> validation_source_authorization_;
    write_stage stage_ = write_stage::payload;
    bool pg_poisoned_ = false;

    [[nodiscard]] static bool checked_add_(std::uint64_t& value, std::uint64_t increment) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)() - value) {
            return false;
        }
        value += increment;
        return true;
    }

    void drop_pending_slots_as_cpp_() {
        for (TupleTableSlot* slot : pending_slots_) {
            if (slot != nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [slot] { ExecDropSingleTupleTableSlot(slot); });
                inject_minimizer_generation_failure_as_cpp(
                    minimizer_generation_failure_injection::generation_tts_drop,
                    "generation_tts_drop");
            }
        }
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
    }

    void abandon_resources_noexcept_() noexcept {
        validation_source_authorization_.reset();
        pending_slots_.clear();
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
        validation_scan_ = nullptr;
        validation_primary_index_ = nullptr;
        validation_snapshot_ = nullptr;
        catalog_indexes_ = nullptr;
        relation_ = nullptr;
    }

    void fail_cleanup_() noexcept {
        stage_ = write_stage::failed;
        pg_poisoned_ = true;
        abandon_resources_noexcept_();
        identity_.release_owned_strings();
    }

    void abandon_after_pg_error_() noexcept {
        stage_ = write_stage::failed;
        pg_poisoned_ = true;
        validation_source_authorization_.reset();
        // Let PostgreSQL reclaim slots, scans, snapshots and relation locks when
        // the statement or transaction aborts. Avoid further PostgreSQL calls
        // while the original error propagates.
        pending_slots_.clear();
        std::vector<TupleTableSlot*>{}.swap(pending_slots_);
        rows_in_batch_ = 0;
        bytes_in_batch_ = 0;
        validation_scan_ = nullptr;
        validation_primary_index_ = nullptr;
        validation_snapshot_ = nullptr;
        catalog_indexes_ = nullptr;
        relation_ = nullptr;
        identity_.release_owned_strings();
    }

    void flush_bounded_batch_as_cpp_() {
        if (pending_slots_.empty()) {
            if (rows_in_batch_ != 0 || bytes_in_batch_ != 0) {
                minimizer_generation_storage_error_as_cpp(
                    "generation write batch counters disagree");
            }
            return;
        }
        if (relation_ == nullptr || catalog_indexes_ == nullptr ||
            pending_slots_.size() != rows_in_batch_ ||
            pending_slots_.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            minimizer_generation_storage_error_as_cpp("generation write batch state is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            CatalogTuplesMultiInsertWithInfo(relation_,
                                             pending_slots_.data(),
                                             static_cast<int>(pending_slots_.size()),
                                             catalog_indexes_);
        });
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::generation_multi_insert,
            "generation_multi_insert");
        if (!checked_add_(catalog_multi_insert_batches_, 1)) {
            minimizer_generation_storage_error_as_cpp("generation write batch counter overflow");
        }
        drop_pending_slots_as_cpp_();
    }

    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    validate_next_row_(const minimizer_generation_row_view& row) noexcept {
        if (stage_ == write_stage::manifest || stage_ == write_stage::tokenized ||
            stage_ == write_stage::failed || stage_ == write_stage::moved) {
            return minimizer_generation_encoding_error::row_order;
        }
        const bool manifest = row.address.position_segment == -1 &&
                              row.address.sub_key == k_directory_sub_key &&
                              row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1 &&
                               row.address.sub_key == k_directory_sub_key &&
                               row.address.chunk_no > 0;
        if (manifest) {
            if (row.address.chunk_no != 0 ||
                (row.bytes.size() != minimizer_generation_manifest::encoded_bytes &&
                 row.bytes.size() != minimizer_generation_manifest::encoded_bytes)) {
                return minimizer_generation_encoding_error::row_order;
            }
            return std::nullopt;
        }
        if (directory) {
            if (stage_ != write_stage::payload && stage_ != write_stage::directory) {
                return minimizer_generation_encoding_error::row_order;
            }
            if (row.address.chunk_no != next_directory_chunk_) {
                return minimizer_generation_encoding_error::chunk_range;
            }
            return std::nullopt;
        }
        if (stage_ != write_stage::payload || row.address.position_segment < 0 ||
            row.address.sub_key < 0 || row.address.sub_key >= k_temporary_run_sub_key_base) {
            return minimizer_generation_encoding_error::row_order;
        }
        const bool position_extent = row.address.sub_key >= k_position_extent_sub_key_base;
        const index_sub_key_t logical_sub_key =
            position_extent ? row.address.sub_key - k_position_extent_sub_key_base
                            : row.address.sub_key;
        if (logical_sub_key < 0 || logical_sub_key >= k_position_extent_sub_key_base) {
            return minimizer_generation_encoding_error::sub_key;
        }
        if (previous_payload_address_) {
            const auto previous_global = std::tuple{previous_payload_address_->sub_key,
                                                    previous_payload_address_->position_segment,
                                                    previous_payload_address_->chunk_no};
            const auto current_global =
                std::tuple{row.address.sub_key, row.address.position_segment, row.address.chunk_no};
            global_payload_order_possible_ =
                global_payload_order_possible_ && previous_global < current_global;
            if (!global_payload_order_possible_) {
                return minimizer_generation_encoding_error::row_order;
            }
        }
        return std::nullopt;
    }

    void record_written_row_(const minimizer_generation_row_view& row,
                             std::uint64_t admitted_batch_bytes) {
        const bool manifest = row.address.position_segment == -1 &&
                              row.address.sub_key == k_directory_sub_key &&
                              row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1 &&
                               row.address.sub_key == k_directory_sub_key &&
                               row.address.chunk_no > 0;
        if (manifest) {
            written_manifest_bytes_.fill(std::byte{0});
            std::copy(row.bytes.begin(), row.bytes.end(), written_manifest_bytes_.begin());
            written_manifest_size_ = static_cast<std::uint32_t>(row.bytes.size());
            stage_ = write_stage::manifest;
        } else if (directory) {
            stage_ = write_stage::directory;
            ++next_directory_chunk_;
            directory_checksum_.update(row.bytes);
            if (!checked_add_(directory_bytes_written_, row.bytes.size())) {
                minimizer_generation_storage_error_as_cpp("directory byte counter overflow");
            }
        } else {
            previous_payload_address_ = row.address;
            payload_row_checksums_.update_little_endian(row.row_checksum);
            if (!checked_add_(payload_rows_written_, 1) ||
                !checked_add_(payload_bytes_written_, row.bytes.size())) {
                minimizer_generation_storage_error_as_cpp("payload write counter overflow");
            }
        }
        if (!checked_add_(row_count_, 1) || !checked_add_(rows_in_batch_, 1) ||
            !checked_add_(bytes_in_batch_, admitted_batch_bytes)) {
            minimizer_generation_storage_error_as_cpp("generation write counter overflow");
        }
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    write_validated_row_(const validated_minimizer_generation_row& validated_row) {
        const minimizer_generation_row_view& row = validated_row.row();
        try {
            auto tuple = form_minimizer_generation_row_pg(relation_, query_key_, validated_row);
            if (!tuple) {
                fail_cleanup_();
                return std::unexpected(tuple.error());
            }
            const std::uint64_t actual_batch_bytes = static_cast<std::uint64_t>((*tuple)->t_len) +
                                                     sizeof(TupleTableSlot*) +
                                                     sizeof(HeapTupleData);
            const auto batch_decision =
                bioseq::indexing::plan_index_tuple_batch_append(pending_slots_.size(),
                                                                bytes_in_batch_,
                                                                actual_batch_bytes,
                                                                write_batch_row_limit_,
                                                                k_write_batch_bytes);
            if (!batch_decision.valid) {
                heap_freetuple(*tuple);
                minimizer_generation_storage_error_as_cpp(
                    "generation write row exceeds bounded catalog batch");
            }
            if (batch_decision.flush_before) {
                flush_bounded_batch_as_cpp_();
            }

            TupleTableSlot* slot = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                return MakeSingleTupleTableSlot(RelationGetDescr(relation_), &TTSOpsHeapTuple);
            });
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_tts_create,
                "generation_tts_create");
            try {
                pending_slots_.push_back(slot);
            } catch (...) {
                heap_freetuple(*tuple);
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [slot] { ExecDropSingleTupleTableSlot(slot); });
                throw;
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [formed_tuple = *tuple, slot] { ExecStoreHeapTuple(formed_tuple, slot, true); });
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_tts_store,
                "generation_tts_store");
            record_written_row_(row, actual_batch_bytes);
            if (batch_decision.flush_after) {
                flush_bounded_batch_as_cpp_();
            }
        } catch (const bioseq::postgres::support::postgres_error&) {
            abandon_after_pg_error_();
            throw;
        }
        return {};
    }

  public:
    index_storage_fragment_generation_row_sink() = delete;
    index_storage_fragment_generation_row_sink(
        minimizer_generation_administration_identity identity,
        std::uint64_t query_key,
        std::uint64_t generation_id,
        std::uint64_t write_batch_row_limit = k_write_batch_rows)
        : identity_(std::move(identity)), query_key_(query_key), generation_id_(generation_id),
          write_batch_row_limit_(write_batch_row_limit) {
        try {
            if (!bioseq::indexing::sql_bigint_can_represent(query_key) || generation_id == 0 ||
                !bioseq::indexing::sql_bigint_can_represent(generation_id) ||
                identity_.database_oid != MyDatabaseId || !OidIsValid(identity_.user_relid) ||
                identity_.column_attno <= 0 || identity_.index_name.empty() ||
                identity_.table_name.empty() || write_batch_row_limit_ == 0 ||
                write_batch_row_limit_ > k_write_batch_rows) {
                minimizer_generation_storage_error_as_cpp(
                    "generation sink identity/key is invalid");
            }
            advisory_lock_key_ = index_advisory_xact_lock_key(identity_.database_oid,
                                                              identity_.tablespace_oid,
                                                              identity_.main_relfilenode,
                                                              identity_.column_attno,
                                                              identity_.index_name);
            bioseq::postgres::support::invoke_pg_as_cpp([this] {
                acquire_index_advisory_xact_lock(identity_.database_oid,
                                                 identity_.tablespace_oid,
                                                 identity_.main_relfilenode,
                                                 identity_.column_attno,
                                                 identity_.index_name);
            });
            const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                return get_relname_relid(identity_.table_name.c_str(), bioseq_internal_ns());
            });
            relation_ = OidIsValid(relation_oid)
                            ? bioseq::postgres::support::invoke_pg_as_cpp([relation_oid] {
                                  return bioseq::postgres::support::try_open_postgres_relation(
                                      relation_oid, RowExclusiveLock);
                              })
                            : nullptr;
            if (relation_ == nullptr || RelationGetNamespace(relation_) != bioseq_internal_ns() ||
                std::string_view{RelationGetRelationName(relation_)} != identity_.table_name) {
                minimizer_generation_storage_error_as_cpp(
                    "generation relation is missing or renamed");
            }
            const minimizer_generation_schema_inspection schema =
                bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return inspect_minimizer_generation_schema(
                        relation_,
                        nullptr,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        });
                });
            if (!schema) {
                minimizer_generation_storage_error_as_cpp(
                    fmt::format("generation relation schema mismatch: {}",
                                minimizer_generation_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(relation_);
            catalog_indexes_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return CatalogOpenIndexes(relation_); });
            if (catalog_indexes_ == nullptr) {
                minimizer_generation_storage_error_as_cpp("CatalogOpenIndexes returned NULL");
            }
        } catch (const bioseq::postgres::support::postgres_error&) {
            abandon_after_pg_error_();
            throw;
        }
    }
    index_storage_fragment_generation_row_sink(const index_storage_fragment_generation_row_sink&) =
        delete;
    index_storage_fragment_generation_row_sink&
    operator=(const index_storage_fragment_generation_row_sink&) = delete;
    index_storage_fragment_generation_row_sink(
        index_storage_fragment_generation_row_sink&& other) noexcept
        : identity_(std::move(other.identity_)), relation_(std::exchange(other.relation_, nullptr)),
          catalog_indexes_(std::exchange(other.catalog_indexes_, nullptr)),
          validation_primary_index_(std::exchange(other.validation_primary_index_, nullptr)),
          validation_snapshot_(std::exchange(other.validation_snapshot_, nullptr)),
          validation_scan_(std::exchange(other.validation_scan_, nullptr)),
          advisory_lock_key_(other.advisory_lock_key_), query_key_(other.query_key_),
          generation_id_(other.generation_id_), row_count_(other.row_count_),
          write_batch_row_limit_(other.write_batch_row_limit_),
          catalog_multi_insert_batches_(other.catalog_multi_insert_batches_),
          pending_slots_(std::move(other.pending_slots_)), rows_in_batch_(other.rows_in_batch_),
          bytes_in_batch_(other.bytes_in_batch_),
          payload_rows_written_(other.payload_rows_written_),
          payload_bytes_written_(other.payload_bytes_written_),
          payload_row_checksums_(other.payload_row_checksums_),
          next_directory_chunk_(other.next_directory_chunk_),
          directory_bytes_written_(other.directory_bytes_written_),
          directory_checksum_(other.directory_checksum_),
          previous_payload_address_(other.previous_payload_address_),
          global_payload_order_possible_(other.global_payload_order_possible_),
          written_manifest_bytes_(other.written_manifest_bytes_),
          written_manifest_size_(other.written_manifest_size_),
          validation_source_authorization_(std::move(other.validation_source_authorization_)),
          stage_(other.stage_), pg_poisoned_(other.pg_poisoned_) {
        other.stage_ = write_stage::moved;
        other.pg_poisoned_ = true;
    }
    index_storage_fragment_generation_row_sink&
    operator=(index_storage_fragment_generation_row_sink&&) = delete;
    ~index_storage_fragment_generation_row_sink() {
        abandon_resources_noexcept_();
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    write(const minimizer_generation_row_view& row) {
        if (pg_poisoned_) {
            minimizer_generation_storage_error_as_cpp(
                "poisoned generation row sink reused after PostgreSQL error");
        }
        if (auto order_error = validate_next_row_(row)) {
            fail_cleanup_();
            return std::unexpected(*order_error);
        }
        auto validated = validate_minimizer_generation_row_for_write(generation_id_, row);
        if (!validated) {
            fail_cleanup_();
            return std::unexpected(validated.error());
        }
        return write_validated_row_(*validated);
    }

    /// Validate and write a grouped SoA payload row, computing its CRC once.
    /// Return the checksum so the encoder can include it in the mxnifest.
    [[nodiscard]] std::expected<minimizer_generation_row_write_receipt,
                                minimizer_generation_encoding_error>
    write_grouped_soa_payload(const unchecked_minimizer_generation_row_view& row) {
        if (pg_poisoned_) {
            minimizer_generation_storage_error_as_cpp(
                "poisoned generation row sink reused after PostgreSQL error");
        }
        const minimizer_generation_row_view order_view{
            .address = row.address,
            .bytes = row.bytes,
            .row_checksum = 0,
        };
        if (auto order_error = validate_next_row_(order_view)) {
            fail_cleanup_();
            return std::unexpected(*order_error);
        }
        auto validated =
            minimizer_generation_row_validator::validate_and_checksum(generation_id_, row);
        if (!validated) {
            fail_cleanup_();
            return std::unexpected(validated.error());
        }
        const std::uint64_t checksum = validated->row().row_checksum;
        if (auto written = write_validated_row_(*validated); !written) {
            return std::unexpected(written.error());
        }
        return minimizer_generation_row_write_receipt{
            .row_checksum = checksum,
        };
    }

    [[nodiscard]] std::uint64_t row_count() const noexcept {
        return row_count_;
    }
    [[nodiscard]] std::uint64_t catalog_multi_insert_batches() const noexcept {
        return catalog_multi_insert_batches_;
    }
    [[nodiscard]] ready_minimizer_generation_token
    validate_ready_generation(const encoded_grouped_soa_generation& generation) {
        const auto minimizer_generation_storage_error = [](std::string_view message) {
            minimizer_generation_storage_error_as_cpp(message);
        };
        Oid validated_relation_oid = InvalidOid;
        RelFileLocator validated_locator{};
        std::array<std::byte, minimizer_generation_head::encoded_bytes> validated_head_bytes{};
        std::uint64_t validated_head_checksum = 0;
        std::uint64_t validated_source_identity = 0;
        std::uint64_t validated_source_content_checksum = 0;
        std::uint64_t validated_storage_contract_checksum = 0;
        std::optional<minimizer_fragment_image_file_receipt> validated_file_receipt{};

        try {
            if (stage_ != write_stage::manifest || relation_ == nullptr ||
                catalog_indexes_ == nullptr || generation.head.generation_id != generation_id_ ||
                generation.manifest.generation_id != generation_id_) {
                minimizer_generation_storage_error("READY validation identity/stage disagrees");
            }
            if (auto metadata = bioseq::indexing::validate_minimizer_generation_metadata(
                    generation.head, generation.manifest, generation.directory);
                !metadata) {
                minimizer_generation_storage_error("READY validation metadata disagrees");
            }
            const bool mapped_minimizer_fragment_image =
                identity_.index_name == k_minimizers_per_fragment_index_name;
            std::unique_ptr<minimizer_fragment_image_writer> fragment_image_writer;
            /* Only minimizer-fragment indexes use this mapped file format.
             * Minimizer-sequence indexes keep  their payload in PostgreSQL, so
             * they must leave the fragment index file alone */
            if (mapped_minimizer_fragment_image) {
                fragment_image_writer = std::make_unique<minimizer_fragment_image_writer>(
                    identity_, query_key_, generation_id_);
            }
            if (!global_payload_order_possible_) {
                minimizer_generation_storage_error(
                    "sink payload order disagrees with grouped-SoA storage");
            }
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(generation.manifest.layout);
            if (extent_payload_bytes == 0) {
                minimizer_generation_storage_error("READY validation wire quantum is invalid");
            }
            const minimizer_generation_row_view head_row{
                .address = generation.unpublished_head_row.address,
                .bytes = generation.unpublished_head_row.bytes(),
                .row_checksum = generation.unpublished_head_row.row_checksum,
            };
            if (head_row.address !=
                    index_chunk_address{
                        .generation_id = 0,
                        .position_segment = -1,
                        .sub_key = k_generation_head_sub_key,
                        .chunk_no = 0,
                    } ||
                head_row.bytes.size() != validated_head_bytes.size() ||
                head_row.row_checksum != bioseq::indexing::crc64_index_chunk_row(
                                             0,
                                             -1,
                                             k_generation_head_sub_key,
                                             0,
                                             static_cast<std::uint32_t>(head_row.bytes.size()),
                                             head_row.bytes)) {
                minimizer_generation_storage_error("READY validation head row is malformed");
            }
            std::copy(head_row.bytes.begin(), head_row.bytes.end(), validated_head_bytes.begin());
            validated_head_checksum = head_row.row_checksum;

            std::uint64_t expected_payload_rows = 0;
            std::uint64_t expected_payload_bytes = 0;
            std::optional<std::pair<index_sub_key_t, std::int32_t>> previous_storage_key;
            for (const grouped_soa_bucket_directory_entry& entry : generation.directory.entries()) {
                const std::pair storage_key{entry.sub_key, entry.position_segment};
                if (previous_storage_key && !(*previous_storage_key < storage_key)) {
                    minimizer_generation_storage_error(
                        "directory is not in physical PK storage order");
                }
                previous_storage_key = storage_key;
                if (!checked_add_(expected_payload_rows,
                                  static_cast<std::uint64_t>(entry.control_chunk_count) +
                                      entry.position_chunk_count) ||
                    !checked_add_(expected_payload_bytes, entry.control_encoded_bytes) ||
                    !checked_add_(expected_payload_bytes, entry.position_encoded_bytes)) {
                    minimizer_generation_storage_error("READY expected payload counters overflow");
                }
            }
            std::uint64_t expected_total_rows = expected_payload_rows;
            if (!checked_add_(expected_total_rows, generation.manifest.directory_chunk_count) ||
                !checked_add_(expected_total_rows, 1)) {
                minimizer_generation_storage_error("READY expected row counter overflow");
            }
            if (row_count_ != expected_total_rows ||
                payload_rows_written_ != expected_payload_rows ||
                payload_bytes_written_ != expected_payload_bytes ||
                payload_row_checksums_.checksum() != generation.manifest.payload_checksum ||
                next_directory_chunk_ != generation.manifest.directory_chunk_count + 1u ||
                directory_bytes_written_ != generation.manifest.directory_bytes ||
                directory_checksum_.checksum() != generation.manifest.directory_checksum ||
                written_manifest_size_ != generation.manifest_row.bytes().size() ||
                !std::ranges::equal(std::span<const std::byte>{written_manifest_bytes_}.first(
                                        written_manifest_size_),
                                    generation.manifest_row.bytes())) {
                minimizer_generation_storage_error("sink write order/count/checksum disagrees");
            }

            flush_bounded_batch_as_cpp_();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { CatalogCloseIndexes(catalog_indexes_); });
            catalog_indexes_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
            if (!bioseq::postgres::support::invoke_pg_as_cpp([] { return ActiveSnapshotSet(); })) {
                minimizer_generation_storage_error("READY reread requires an active MVCC snapshot");
            }
            Snapshot active_snapshot =
                bioseq::postgres::support::invoke_pg_as_cpp([] { return GetActiveSnapshot(); });
            if (!IsMVCCSnapshot(active_snapshot)) {
                minimizer_generation_storage_error("READY reread active snapshot is not MVCC");
            }
            validation_snapshot_ = register_minimizer_reread_snapshot_pg(active_snapshot);
            if (validation_snapshot_->curcid != bioseq::postgres::support::invoke_pg_as_cpp(
                                               [] { return GetCurrentCommandId(false); })) {
                minimizer_generation_storage_error(
                    "READY reread private command ID was not refreshed");
            }
            if (validation_snapshot_ == nullptr || validation_snapshot_->regd_count == 0) {
                minimizer_generation_storage_error("READY reread snapshot was not registered");
            }

            validation_source_authorization_.emplace(authorize_minimizer_generation_source_pg(
                identity_, query_key_, validation_snapshot_));
            minimizer_generation_source_authorization& source_authorization =
                *validation_source_authorization_;
            if (!source_authorization.valid_ ||
                source_authorization.snapshot_ != validation_snapshot_ ||
                source_authorization.database_oid_ != identity_.database_oid ||
                source_authorization.tablespace_oid_ != identity_.tablespace_oid ||
                source_authorization.main_relfilenode_ != identity_.main_relfilenode ||
                source_authorization.user_relid_ != identity_.user_relid ||
                source_authorization.column_attno_ != identity_.column_attno ||
                source_authorization.index_name_ != identity_.index_name ||
                source_authorization.table_name_ != identity_.table_name ||
                source_authorization.source_.sequence_rowno != query_key_) {
                minimizer_generation_storage_error(
                    "READY authoritative source authorization binding disagrees");
            }
            const std::uint64_t storage_contract_checksum =
                bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    generation.manifest);
            if (generation.manifest.alphabet_code != source_authorization.source_.alphabet_code ||
                generation.manifest.storage_unit_byte_width !=
                    source_authorization.source_.storage_unit_byte_width ||
                generation.manifest.target_length !=
                    source_authorization.source_.locator.logical_length ||
                generation.head.storage_contract_checksum != storage_contract_checksum ||
                generation.head.source_content_checksum !=
                    source_authorization.source_content_checksum_ ||
                generation.manifest.source_content_checksum !=
                    source_authorization.source_content_checksum_) {
                minimizer_generation_storage_error(
                    "READY authoritative source content/storage contract disagrees");
            }
            source_authorization.source_.storage_contract_checksum = storage_contract_checksum;
            const std::uint64_t required_source_identity = minimizer_generation_source_identity(
                identity_.main_relfilenode, source_authorization.source_);
            if (required_source_identity == 0 ||
                generation.head.source_identity != required_source_identity ||
                generation.manifest.source_identity != required_source_identity) {
                minimizer_generation_storage_error("READY authoritative source identity disagrees");
            }
            validated_source_identity = required_source_identity;
            validated_source_content_checksum = source_authorization.source_content_checksum_;
            validated_storage_contract_checksum = storage_contract_checksum;
            source_authorization.invalidate_();
            validation_source_authorization_.reset();

            const minimizer_generation_schema_inspection schema =
                bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return inspect_minimizer_generation_schema(
                        relation_,
                        nullptr,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        });
                });
            if (!schema) {
                minimizer_generation_storage_error("READY relation schema changed");
            }
            validated_relation_oid = RelationGetRelid(relation_);
            validated_locator = relation_->rd_locator;
            validation_primary_index_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [&schema] { return index_open(schema.primary_index_oid, AccessShareLock); });

            ScanKeyData metadata_keys[4];
            ScanKeyInit(&metadata_keys[0],
                        k_minimizer_generation_index_generation_id,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&metadata_keys[1],
                        k_minimizer_generation_index_query_key,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&metadata_keys[2],
                        k_minimizer_generation_index_sub_key,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        Int64GetDatum(static_cast<std::int64_t>(k_directory_sub_key)));
            ScanKeyInit(&metadata_keys[3],
                        k_minimizer_generation_index_position_segment,
                        BTEqualStrategyNumber,
                        F_INT4EQ,
                        Int32GetDatum(-1));
            validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &metadata_keys] {
                return systable_beginscan_ordered(
                    relation_, validation_primary_index_, validation_snapshot_, 4, metadata_keys);
            });
            const auto next_validation_tuple = [this] {
                return bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return systable_getnext_ordered(validation_scan_, ForwardScanDirection);
                });
            };
            HeapTuple tuple = next_validation_tuple();
            if (!HeapTupleIsValid(tuple)) {
                minimizer_generation_storage_error("READY manifest row is missing");
            }
            minimizer_generation_loaded_row manifest_row =
                load_minimizer_generation_row_or_error(relation_, tuple);
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_reread, "generation_reread");
            if (manifest_row.query_key != query_key_ ||
                manifest_row.address != generation.manifest_row.address ||
                manifest_row.checksum != generation.manifest_row.row_checksum ||
                !std::ranges::equal(manifest_row.bytes, generation.manifest_row.bytes())) {
                manifest_row.release();
                minimizer_generation_storage_error("READY manifest reread disagrees");
            }
            manifest_row.release();

            crc64_ecma182_accumulator reread_directory_checksum;
            std::uint64_t reread_directory_bytes = 0;
            const std::span<const std::byte> expected_directory =
                generation.directory.encoded_bytes();
            for (std::uint32_t chunk_no = 1; chunk_no <= generation.manifest.directory_chunk_count;
                 ++chunk_no) {
                tuple = next_validation_tuple();
                if (!HeapTupleIsValid(tuple)) {
                    minimizer_generation_storage_error("READY directory chunk is missing");
                }
                minimizer_generation_loaded_row directory_row =
                    load_minimizer_generation_row_or_error(relation_, tuple);
                const std::uint64_t offset64 =
                    static_cast<std::uint64_t>(chunk_no - 1u) * extent_payload_bytes;
                const std::size_t offset = static_cast<std::size_t>(offset64);
                const std::size_t count =
                    std::min<std::size_t>(extent_payload_bytes, expected_directory.size() - offset);
                const auto expected_bytes = expected_directory.subspan(offset, count);
                if (directory_row.query_key != query_key_ ||
                    directory_row.address !=
                        index_chunk_address{
                            .generation_id = generation_id_,
                            .position_segment = -1,
                            .sub_key = k_directory_sub_key,
                            .chunk_no = chunk_no,
                        } ||
                    !std::ranges::equal(directory_row.bytes, expected_bytes)) {
                    directory_row.release();
                    minimizer_generation_storage_error("READY directory reread disagrees");
                }
                reread_directory_checksum.update(directory_row.bytes);
                if (!checked_add_(reread_directory_bytes, directory_row.bytes.size())) {
                    directory_row.release();
                    minimizer_generation_storage_error("READY directory reread bytes overflow");
                }
                directory_row.release();
            }
            if (HeapTupleIsValid(next_validation_tuple())) {
                minimizer_generation_storage_error("READY directory reread has extra rows");
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(validation_scan_); });
            validation_scan_ = nullptr;
            if (reread_directory_bytes != generation.manifest.directory_bytes ||
                reread_directory_checksum.checksum() != generation.manifest.directory_checksum) {
                minimizer_generation_storage_error("READY directory aggregate reread disagrees");
            }

            ScanKeyData payload_keys[3];
            ScanKeyInit(&payload_keys[0],
                        k_minimizer_generation_index_generation_id,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&payload_keys[1],
                        k_minimizer_generation_index_query_key,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&payload_keys[2],
                        k_minimizer_generation_index_position_segment,
                        BTGreaterEqualStrategyNumber,
                        F_INT4GE,
                        Int32GetDatum(0));
            validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp([this, &payload_keys] {
                return systable_beginscan_ordered(
                    relation_, validation_primary_index_, validation_snapshot_, 3, payload_keys);
            });
            const auto directory_entries = generation.directory.entries();
            std::size_t entry_ordinal = 0;
            std::uint32_t entry_chunk = 0;
            bool position_extent = false;
            bool payload_complete = directory_entries.empty();
            std::uint64_t reread_payload_rows = 0;
            std::uint64_t reread_payload_bytes = 0;
            crc64_ecma182_accumulator extent_checksum;
            for (tuple = next_validation_tuple(); HeapTupleIsValid(tuple);
                 tuple = next_validation_tuple()) {
                if (payload_complete || entry_ordinal >= directory_entries.size()) {
                    minimizer_generation_storage_error("READY payload reread has extra rows");
                }
                const grouped_soa_bucket_directory_entry& entry = directory_entries[entry_ordinal];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    minimizer_generation_storage_error(
                        "READY directory position sub-key is invalid");
                }
                const std::uint32_t extent_chunk_count =
                    position_extent ? entry.position_chunk_count : entry.control_chunk_count;
                const std::uint64_t extent_bytes =
                    position_extent ? entry.position_encoded_bytes : entry.control_encoded_bytes;
                const std::uint64_t offset =
                    static_cast<std::uint64_t>(entry_chunk) * extent_payload_bytes;
                const std::uint64_t expected_bytes =
                    std::min<std::uint64_t>(extent_payload_bytes, extent_bytes - offset);
                minimizer_generation_loaded_row payload_row =
                    load_minimizer_generation_row_or_error(relation_, tuple);
                if (payload_row.query_key != query_key_ ||
                    payload_row.address !=
                        index_chunk_address{
                            .generation_id = generation_id_,
                            .position_segment = entry.position_segment,
                            .sub_key = position_extent ? *position_sub_key : entry.sub_key,
                            .chunk_no = entry_chunk,
                        } ||
                    payload_row.bytes.size() != expected_bytes) {
                    payload_row.release();
                    minimizer_generation_storage_error("READY payload row/address/size disagrees");
                }
                extent_checksum.update(payload_row.bytes);
                if (!checked_add_(reread_payload_rows, 1) ||
                    !checked_add_(reread_payload_bytes, payload_row.bytes.size())) {
                    payload_row.release();
                    minimizer_generation_storage_error("READY payload reread counters overflow");
                }
                if (fragment_image_writer) {
                    fragment_image_writer->append(payload_row.bytes);
                }
                payload_row.release();
                ++entry_chunk;
                if (entry_chunk == extent_chunk_count) {
                    const std::uint64_t expected_extent_checksum =
                        position_extent ? entry.position_checksum : entry.control_checksum;
                    if (extent_checksum.checksum() != expected_extent_checksum) {
                        minimizer_generation_storage_error(
                            position_extent ? "READY position extent checksum disagrees"
                                            : "READY control extent checksum disagrees");
                    }
                    extent_checksum = {};
                    entry_chunk = 0;
                    ++entry_ordinal;
                    if (entry_ordinal == directory_entries.size()) {
                        if (!position_extent) {
                            position_extent = true;
                            entry_ordinal = 0;
                        } else {
                            payload_complete = true;
                        }
                    }
                }
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { systable_endscan_ordered(validation_scan_); });
            validation_scan_ = nullptr;
            // The primary key orders all control rows before all position rows.
            // This reread checks each extent and the total size. The checksum
            // of emitted rows was already checked before the reread.
            if (!payload_complete || entry_chunk != 0 ||
                reread_payload_rows != expected_payload_rows ||
                reread_payload_bytes != expected_payload_bytes ||
                reread_payload_bytes != generation.manifest.payload_bytes) {
                minimizer_generation_storage_error(
                    "READY complete payload aggregate reread disagrees");
            }
            if (fragment_image_writer) {
                validated_file_receipt =
                    fragment_image_writer->publish(generation.head,
                                                   generation.manifest,
                                                   generation.directory,
                                                   generation.manifest_row.bytes());
            }

            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { index_close(validation_primary_index_, AccessShareLock); });
            validation_primary_index_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { UnregisterSnapshot(validation_snapshot_); });
            validation_snapshot_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { relation_close(relation_, RowExclusiveLock); });
            relation_ = nullptr;
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_writer_close,
                "generation_writer_close");
        } catch (const bioseq::postgres::support::postgres_error&) {
            abandon_after_pg_error_();
            throw;
        }

        stage_ = write_stage::tokenized;
        const TransactionId validated_transaction_id =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetCurrentTransactionId(); });
        minimizer_generation_administration_identity validated_identity = std::move(identity_);
        return ready_minimizer_generation_token{std::move(validated_identity),
                                                validated_relation_oid,
                                                validated_locator,
                                                advisory_lock_key_,
                                                query_key_,
                                                generation_id_,
                                                validated_source_identity,
                                                validated_source_content_checksum,
                                                validated_storage_contract_checksum,
                                                validated_transaction_id,
                                                validated_head_bytes,
                                                validated_head_checksum,
                                                std::move(validated_file_receipt)};
    }
};

static_assert(
    bioseq::indexing::minimizer_generation_row_sink<index_storage_fragment_generation_row_sink>);
static_assert(bioseq::indexing::grouped_soa_minimizer_payload_sink<
              index_storage_fragment_generation_row_sink>);
static_assert(!std::is_default_constructible_v<index_storage_fragment_generation_row_sink>);
static_assert(!std::is_copy_constructible_v<index_storage_fragment_generation_row_sink>);
static_assert(std::is_nothrow_move_constructible_v<index_storage_fragment_generation_row_sink>);
static_assert(!std::is_move_assignable_v<index_storage_fragment_generation_row_sink>);

} // namespace bioseq::storage::indexing
