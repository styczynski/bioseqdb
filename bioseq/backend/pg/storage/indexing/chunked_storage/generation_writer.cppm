/**
 * @file generation_writer.cppm
 * @brief Relation-owned grouped-SoA generation writer and atomic head
 *        publication.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`; the table layout shards via `index_table_shard`.
 */
module;
#include <libassert/assert.hpp>
#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "common/relpath.h"
#include "commands/sequence.h"
#include "miscadmin.h"
#include "catalog/namespace.h"   // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "port/atomics.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"
#include "port.h"
}

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <expected>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.postgres.storage.indexing.chunked_storage:generation_writer;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support;                       // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager;                 // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;             // sequence_locator
import bioseq.storage.size;            // sequence_size_t
import bioseq.indexing.type;           // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;        // the concepts
import bioseq.indexing.minimizer_hashmap;   // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image;       // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io;                // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;      // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;            // lazy_sequence
import bioseq.postgres.types.compact_sequence;  // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;               // compact_sequence_range
import bioseq.sequences.simd;          // canonical bounded base decoding
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
import :file_actions;
import :fragment_image;
export namespace bioseq::storage::indexing {

/// Relation-owning sink factory target used directly by the existing common
/// three-scan generation encoder. It cannot publish the head and therefore cannot
/// make an incompletely reread generation visible.
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
    static constexpr std::uint64_t k_write_batch_bytes =
        MAX_CATALOG_MULTI_INSERT_BYTES;

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
    std::array<std::byte, minimizer_generation_manifest::encoded_bytes>
        written_manifest_bytes_{};
    std::uint32_t written_manifest_size_ = 0;
    std::optional<minimizer_generation_source_authorization> validation_source_authorization_;
    write_stage stage_ = write_stage::payload;
    bool pg_poisoned_ = false;

    [[nodiscard]] static bool checked_add_(
            std::uint64_t& value,
            std::uint64_t increment) noexcept {
        if (increment
            > (std::numeric_limits<std::uint64_t>::max)() - value) {
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
        // Do not call any PostgreSQL cleanup routine while propagating an
        // ERROR converted at a leaf. Slots, scans, snapshots, catalog state
        // and relation locks remain registered with PostgreSQL's current
        // ResourceOwner and are reclaimed by statement/transaction abort.
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
        if (relation_ == nullptr || catalog_indexes_ == nullptr
            || pending_slots_.size() != rows_in_batch_
            || pending_slots_.size()
                > static_cast<std::size_t>(
                    (std::numeric_limits<int>::max)())) {
            minimizer_generation_storage_error_as_cpp(
                "generation write batch state is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            CatalogTuplesMultiInsertWithInfo(
                relation_, pending_slots_.data(),
                static_cast<int>(pending_slots_.size()), catalog_indexes_);
        });
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::generation_multi_insert,
            "generation_multi_insert");
        if (!checked_add_(catalog_multi_insert_batches_, 1)) {
            minimizer_generation_storage_error_as_cpp(
                "generation write batch counter overflow");
        }
        drop_pending_slots_as_cpp_();
    }

    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    validate_next_row_(const minimizer_generation_row_view& row) noexcept {
        if (stage_ == write_stage::manifest
            || stage_ == write_stage::tokenized
            || stage_ == write_stage::failed
            || stage_ == write_stage::moved) {
            return minimizer_generation_encoding_error::row_order;
        }
        const bool manifest = row.address.position_segment == -1
            && row.address.sub_key == k_directory_sub_key
            && row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1
            && row.address.sub_key == k_directory_sub_key
            && row.address.chunk_no > 0;
        if (manifest) {
            if (row.address.chunk_no != 0
                || (row.bytes.size()
                        != minimizer_generation_manifest::encoded_bytes
                    && row.bytes.size()
                        != minimizer_generation_manifest::
                            encoded_bytes)) {
                return minimizer_generation_encoding_error::row_order;
            }
            return std::nullopt;
        }
        if (directory) {
            if (stage_ != write_stage::payload
                && stage_ != write_stage::directory) {
                return minimizer_generation_encoding_error::row_order;
            }
            if (row.address.chunk_no != next_directory_chunk_) {
                return minimizer_generation_encoding_error::chunk_range;
            }
            return std::nullopt;
        }
        if (stage_ != write_stage::payload
            || row.address.position_segment < 0
            || row.address.sub_key < 0
            || row.address.sub_key >= k_temporary_run_sub_key_base) {
            return minimizer_generation_encoding_error::row_order;
        }
        const bool position_extent =
            row.address.sub_key >= k_position_extent_sub_key_base;
        const index_sub_key_t logical_sub_key = position_extent
            ? row.address.sub_key - k_position_extent_sub_key_base
            : row.address.sub_key;
        if (logical_sub_key < 0
            || logical_sub_key >= k_position_extent_sub_key_base) {
            return minimizer_generation_encoding_error::sub_key;
        }
        if (previous_payload_address_) {
            const auto previous_global = std::tuple{
                previous_payload_address_->sub_key,
                previous_payload_address_->position_segment,
                previous_payload_address_->chunk_no};
            const auto current_global = std::tuple{
                row.address.sub_key,
                row.address.position_segment,
                row.address.chunk_no};
            global_payload_order_possible_ =
                global_payload_order_possible_
                && previous_global < current_global;
            if (!global_payload_order_possible_) {
                return minimizer_generation_encoding_error::row_order;
            }
        }
        return std::nullopt;
    }

    void record_written_row_(
            const minimizer_generation_row_view& row,
            std::uint64_t admitted_batch_bytes) {
        const bool manifest = row.address.position_segment == -1
            && row.address.sub_key == k_directory_sub_key
            && row.address.chunk_no == 0;
        const bool directory = row.address.position_segment == -1
            && row.address.sub_key == k_directory_sub_key
            && row.address.chunk_no > 0;
        if (manifest) {
            written_manifest_bytes_.fill(std::byte{0});
            std::copy(row.bytes.begin(), row.bytes.end(),
                written_manifest_bytes_.begin());
            written_manifest_size_ = static_cast<std::uint32_t>(
                row.bytes.size());
            stage_ = write_stage::manifest;
        } else if (directory) {
            stage_ = write_stage::directory;
            ++next_directory_chunk_;
            directory_checksum_.update(row.bytes);
            if (!checked_add_(directory_bytes_written_, row.bytes.size())) {
                minimizer_generation_storage_error_as_cpp(
                    "directory byte counter overflow");
            }
        } else {
            previous_payload_address_ = row.address;
            payload_row_checksums_.update_little_endian(row.row_checksum);
            if (!checked_add_(payload_rows_written_, 1)
                || !checked_add_(payload_bytes_written_, row.bytes.size())) {
                minimizer_generation_storage_error_as_cpp(
                    "payload write counter overflow");
            }
        }
        if (!checked_add_(row_count_, 1)
            || !checked_add_(rows_in_batch_, 1)
            || !checked_add_(bytes_in_batch_, admitted_batch_bytes)) {
            minimizer_generation_storage_error_as_cpp(
                "generation write counter overflow");
        }
    }

    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    write_validated_row_(
            const validated_minimizer_generation_row& validated_row) {
        const minimizer_generation_row_view& row = validated_row.row();
        try {
            auto tuple = form_minimizer_generation_row_pg(
                relation_, query_key_, validated_row);
            if (!tuple) {
                fail_cleanup_();
                return std::unexpected(tuple.error());
            }
            const std::uint64_t actual_batch_bytes =
                static_cast<std::uint64_t>((*tuple)->t_len)
                + sizeof(TupleTableSlot*) + sizeof(HeapTupleData);
            const auto batch_decision =
                bioseq::indexing::plan_index_tuple_batch_append(
                    pending_slots_.size(), bytes_in_batch_,
                    actual_batch_bytes,
                    write_batch_row_limit_, k_write_batch_bytes);
            if (!batch_decision.valid) {
                heap_freetuple(*tuple);
                minimizer_generation_storage_error_as_cpp(
                    "generation write row exceeds bounded catalog batch");
            }
            if (batch_decision.flush_before) {
                flush_bounded_batch_as_cpp_();
            }

            TupleTableSlot* slot = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                return MakeSingleTupleTableSlot(
                    RelationGetDescr(relation_), &TTSOpsHeapTuple);
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
                [formed_tuple = *tuple, slot] {
                    ExecStoreHeapTuple(formed_tuple, slot, true);
                });
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
        : identity_(std::move(identity)),
          query_key_(query_key), generation_id_(generation_id),
          write_batch_row_limit_(write_batch_row_limit) {
        try {
            if (!bioseq::indexing::sql_bigint_can_represent(query_key)
                || generation_id == 0
                || !bioseq::indexing::sql_bigint_can_represent(generation_id)
                || identity_.database_oid != MyDatabaseId
                || !OidIsValid(identity_.user_relid)
                || identity_.column_attno <= 0
                || identity_.index_name.empty()
                || identity_.table_name.empty()
                || write_batch_row_limit_ == 0
                || write_batch_row_limit_ > k_write_batch_rows) {
                minimizer_generation_storage_error_as_cpp(
                    "generation sink identity/key is invalid");
            }
            advisory_lock_key_ = index_advisory_xact_lock_key(
                identity_.database_oid,
                identity_.tablespace_oid,
                identity_.main_relfilenode,
                identity_.column_attno,
                identity_.index_name);
            bioseq::postgres::support::invoke_pg_as_cpp([this] {
                acquire_index_advisory_xact_lock(
                    identity_.database_oid,
                    identity_.tablespace_oid,
                    identity_.main_relfilenode,
                    identity_.column_attno,
                    identity_.index_name);
            });
            const Oid relation_oid = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                return get_relname_relid(
                    identity_.table_name.c_str(), bioseq_internal_ns());
            });
            relation_ = OidIsValid(relation_oid)
                ? bioseq::postgres::support::invoke_pg_as_cpp([relation_oid] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        relation_oid, RowExclusiveLock);
                })
                : nullptr;
            if (relation_ == nullptr
                || RelationGetNamespace(relation_) != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(relation_)}
                    != identity_.table_name) {
                minimizer_generation_storage_error_as_cpp(
                    "generation relation is missing or renamed");
            }
            const minimizer_generation_schema_inspection schema =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [this] { return inspect_minimizer_generation_schema(
                        relation_, nullptr,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        }); });
            if (!schema) {
                minimizer_generation_storage_error_as_cpp(fmt::format(
                    "generation relation schema mismatch: {}",
                    minimizer_generation_schema_error_name(schema.error)));
            }
            (void)verify_pg_inline_payload_layout(relation_);
            catalog_indexes_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { return CatalogOpenIndexes(relation_); });
            if (catalog_indexes_ == nullptr) {
                minimizer_generation_storage_error_as_cpp(
                    "CatalogOpenIndexes returned NULL");
            }
        } catch (const bioseq::postgres::support::postgres_error&) {
            abandon_after_pg_error_();
            throw;
        }
    }
    index_storage_fragment_generation_row_sink(
        const index_storage_fragment_generation_row_sink&) = delete;
    index_storage_fragment_generation_row_sink& operator=(
        const index_storage_fragment_generation_row_sink&) = delete;
    index_storage_fragment_generation_row_sink(
            index_storage_fragment_generation_row_sink&& other) noexcept
        : identity_(std::move(other.identity_)),
          relation_(std::exchange(other.relation_, nullptr)),
          catalog_indexes_(std::exchange(other.catalog_indexes_, nullptr)),
          validation_primary_index_(std::exchange(
              other.validation_primary_index_, nullptr)),
          validation_snapshot_(std::exchange(
              other.validation_snapshot_, nullptr)),
          validation_scan_(std::exchange(other.validation_scan_, nullptr)),
          advisory_lock_key_(other.advisory_lock_key_),
          query_key_(other.query_key_),
          generation_id_(other.generation_id_),
          row_count_(other.row_count_),
          write_batch_row_limit_(other.write_batch_row_limit_),
          catalog_multi_insert_batches_(
              other.catalog_multi_insert_batches_),
          pending_slots_(std::move(other.pending_slots_)),
          rows_in_batch_(other.rows_in_batch_),
          bytes_in_batch_(other.bytes_in_batch_),
          payload_rows_written_(other.payload_rows_written_),
          payload_bytes_written_(other.payload_bytes_written_),
          payload_row_checksums_(other.payload_row_checksums_),
          next_directory_chunk_(other.next_directory_chunk_),
          directory_bytes_written_(other.directory_bytes_written_),
          directory_checksum_(other.directory_checksum_),
          previous_payload_address_(other.previous_payload_address_),
          global_payload_order_possible_(
              other.global_payload_order_possible_),
          written_manifest_bytes_(other.written_manifest_bytes_),
          written_manifest_size_(other.written_manifest_size_),
          validation_source_authorization_(std::move(
              other.validation_source_authorization_)),
          stage_(other.stage_),
          pg_poisoned_(other.pg_poisoned_) {
        other.stage_ = write_stage::moved;
        other.pg_poisoned_ = true;
    }
    index_storage_fragment_generation_row_sink& operator=(
        index_storage_fragment_generation_row_sink&&) = delete;
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
        auto validated = validate_minimizer_generation_row_for_write(
            generation_id_, row);
        if (!validated) {
            fail_cleanup_();
            return std::unexpected(validated.error());
        }
        return write_validated_row_(*validated);
    }

    /// grouped-SoA payload owner: compute the address-bound CRC once, bind it
    /// into an unforgeable validated token, form the tuple from that token, and
    /// return the exact checksum for manifest folding.
    [[nodiscard]] std::expected<minimizer_generation_row_write_receipt,
        minimizer_generation_encoding_error>
    write_grouped_soa_payload(
            const unchecked_minimizer_generation_row_view& row) {
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
        auto validated = minimizer_generation_row_validator::validate_and_checksum(
            generation_id_, row);
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
    [[nodiscard]] ready_minimizer_generation_token validate_ready_generation(
            const encoded_grouped_soa_generation& generation) {
        const auto minimizer_generation_storage_error = [](std::string_view message) {
            minimizer_generation_storage_error_as_cpp(message);
        };
        Oid validated_relation_oid = InvalidOid;
        RelFileLocator validated_locator{};
        std::array<std::byte, minimizer_generation_head::encoded_bytes>
            validated_head_bytes{};
        std::uint64_t validated_head_checksum = 0;
        std::uint64_t validated_source_identity = 0;
        std::uint64_t validated_source_content_checksum = 0;
        std::uint64_t validated_storage_contract_checksum = 0;
        std::optional<minimizer_fragment_image_file_receipt> validated_file_receipt{};

        try {
            if (stage_ != write_stage::manifest
                || relation_ == nullptr || catalog_indexes_ == nullptr
                || generation.head.generation_id != generation_id_
                || generation.manifest.generation_id != generation_id_) {
                minimizer_generation_storage_error("READY validation identity/stage disagrees");
            }
            if (auto metadata =
                    bioseq::indexing::validate_minimizer_generation_metadata(
                        generation.head,
                        generation.manifest,
                        generation.directory);
                !metadata) {
                minimizer_generation_storage_error("READY validation metadata disagrees");
            }
            const bool mapped_minimizer_fragment_image =
                identity_.index_name == k_minimizers_per_fragment_index_name;
            std::unique_ptr<minimizer_fragment_image_writer>
                fragment_image_writer;
			/* The sidecar path/header intentionally encode the minimizer-fragment
			 * source identity, not a logical-index discriminator. The minimizer-sequence
			 * grouped-SoA route keeps the authoritative PostgreSQL payload and therefore
			 * must not create or overwrite this minimizer-fragment-only image. */
            if (mapped_minimizer_fragment_image) {
                fragment_image_writer =
                    std::make_unique<minimizer_fragment_image_writer>(
                        identity_, query_key_, generation_id_);
            }
            if (!global_payload_order_possible_) {
                minimizer_generation_storage_error(
                    "sink payload order disagrees with grouped-SoA storage");
            }
            const std::uint32_t extent_payload_bytes =
                bioseq::indexing::minimizer_extent_payload_bytes(
                    generation.manifest.layout);
            if (extent_payload_bytes == 0) {
                minimizer_generation_storage_error("READY validation wire quantum is invalid");
            }
            const minimizer_generation_row_view head_row{
                .address = generation.unpublished_head_row.address,
                .bytes = generation.unpublished_head_row.bytes(),
                .row_checksum = generation.unpublished_head_row.row_checksum,
            };
            if (head_row.address != index_chunk_address{
                    .generation_id = 0,
                    .position_segment = -1,
                    .sub_key = k_generation_head_sub_key,
                    .chunk_no = 0,
                }
                || head_row.bytes.size() != validated_head_bytes.size()
                || head_row.row_checksum
                    != bioseq::indexing::crc64_index_chunk_row(
                        0, -1, k_generation_head_sub_key, 0,
                        static_cast<std::uint32_t>(head_row.bytes.size()),
                        head_row.bytes)) {
                minimizer_generation_storage_error("READY validation head row is malformed");
            }
            std::copy(head_row.bytes.begin(), head_row.bytes.end(),
                validated_head_bytes.begin());
            validated_head_checksum = head_row.row_checksum;

            std::uint64_t expected_payload_rows = 0;
            std::uint64_t expected_payload_bytes = 0;
            std::optional<std::pair<index_sub_key_t, std::int32_t>>
                previous_storage_key;
            for (const grouped_soa_bucket_directory_entry& entry
                 : generation.directory.entries()) {
                const std::pair storage_key{
                    entry.sub_key, entry.position_segment};
                if (previous_storage_key
                    && !(*previous_storage_key < storage_key)) {
                    minimizer_generation_storage_error(
                        "directory is not in physical PK storage order");
                }
                previous_storage_key = storage_key;
                if (!checked_add_(expected_payload_rows,
                        static_cast<std::uint64_t>(entry.control_chunk_count)
                            + entry.position_chunk_count)
                    || !checked_add_(expected_payload_bytes,
                        entry.control_encoded_bytes)
                    || !checked_add_(expected_payload_bytes,
                        entry.position_encoded_bytes)) {
                    minimizer_generation_storage_error("READY expected payload counters overflow");
                }
            }
            std::uint64_t expected_total_rows = expected_payload_rows;
            if (!checked_add_(expected_total_rows,
                    generation.manifest.directory_chunk_count)
                || !checked_add_(expected_total_rows, 1)) {
                minimizer_generation_storage_error("READY expected row counter overflow");
            }
            if (row_count_ != expected_total_rows
                || payload_rows_written_ != expected_payload_rows
                || payload_bytes_written_ != expected_payload_bytes
                || payload_row_checksums_.checksum()
                    != generation.manifest.payload_checksum
                || next_directory_chunk_
                    != generation.manifest.directory_chunk_count + 1u
                || directory_bytes_written_
                    != generation.manifest.directory_bytes
                || directory_checksum_.checksum()
                    != generation.manifest.directory_checksum
                || written_manifest_size_
                    != generation.manifest_row.bytes().size()
                || !std::ranges::equal(
                    std::span<const std::byte>{written_manifest_bytes_}
                        .first(written_manifest_size_),
                    generation.manifest_row.bytes())) {
                minimizer_generation_storage_error("sink write order/count/checksum disagrees");
            }

            flush_bounded_batch_as_cpp_();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { CatalogCloseIndexes(catalog_indexes_); });
            catalog_indexes_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { CommandCounterIncrement(); });
            if (!bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return ActiveSnapshotSet(); })) {
                minimizer_generation_storage_error(
                    "READY reread requires an active MVCC snapshot");
            }
            Snapshot active_snapshot = bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetActiveSnapshot(); });
            if (!IsMVCCSnapshot(active_snapshot)) {
                minimizer_generation_storage_error(
                    "READY reread active snapshot is not MVCC");
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { UpdateActiveSnapshotCommandId(); });
            if (active_snapshot->curcid
                != bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetCurrentCommandId(false); })) {
                minimizer_generation_storage_error(
                    "READY reread active command ID was not refreshed");
            }
            validation_snapshot_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [active_snapshot] { return RegisterSnapshot(active_snapshot); });
            if (validation_snapshot_ == nullptr
                || validation_snapshot_->regd_count == 0) {
                minimizer_generation_storage_error("READY reread snapshot was not registered");
            }

            validation_source_authorization_.emplace(
                authorize_minimizer_generation_source_pg(
                    identity_, query_key_, validation_snapshot_));
            minimizer_generation_source_authorization& source_authorization =
                *validation_source_authorization_;
            if (!source_authorization.valid_
                || source_authorization.snapshot_ != validation_snapshot_
                || source_authorization.database_oid_ != identity_.database_oid
                || source_authorization.tablespace_oid_ != identity_.tablespace_oid
                || source_authorization.main_relfilenode_
                    != identity_.main_relfilenode
                || source_authorization.user_relid_ != identity_.user_relid
                || source_authorization.column_attno_ != identity_.column_attno
                || source_authorization.index_name_ != identity_.index_name
                || source_authorization.table_name_ != identity_.table_name
                || source_authorization.source_.sequence_rowno != query_key_) {
                minimizer_generation_storage_error(
                    "READY authoritative source authorization binding disagrees");
            }
            const std::uint64_t storage_contract_checksum =
                bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(
                    generation.manifest);
            if (generation.manifest.alphabet_code
                    != source_authorization.source_.alphabet_code
                || generation.manifest.storage_unit_byte_width
                    != source_authorization.source_.storage_unit_byte_width
                || generation.manifest.target_length
                    != source_authorization.source_.locator.logical_length
                || generation.head.storage_contract_checksum
                    != storage_contract_checksum
                || generation.head.source_content_checksum
                    != source_authorization.source_content_checksum_
                || generation.manifest.source_content_checksum
                    != source_authorization.source_content_checksum_) {
                minimizer_generation_storage_error(
                    "READY authoritative source content/storage contract disagrees");
            }
            source_authorization.source_.storage_contract_checksum =
                storage_contract_checksum;
            const std::uint64_t required_source_identity =
                minimizer_generation_source_identity(
                    identity_.main_relfilenode, source_authorization.source_);
            if (required_source_identity == 0
                || generation.head.source_identity
                    != required_source_identity
                || generation.manifest.source_identity
                    != required_source_identity) {
                minimizer_generation_storage_error(
                    "READY authoritative source identity disagrees");
            }
            validated_source_identity = required_source_identity;
            validated_source_content_checksum =
                source_authorization.source_content_checksum_;
            validated_storage_contract_checksum = storage_contract_checksum;
            source_authorization.invalidate_();
            validation_source_authorization_.reset();

            const minimizer_generation_schema_inspection schema =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [this] { return inspect_minimizer_generation_schema(
                        relation_, nullptr,
                        minimizer_generation_schema_expectation{
                            .namespace_oid = bioseq_internal_ns(),
                            .relation_name = identity_.table_name,
                            .tablespace_oid = identity_.tablespace_oid,
                        }); });
            if (!schema) {
                minimizer_generation_storage_error("READY relation schema changed");
            }
            validated_relation_oid = RelationGetRelid(relation_);
            validated_locator = relation_->rd_locator;
            validation_primary_index_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [&schema] {
                    return index_open(
                        schema.primary_index_oid, AccessShareLock);
                });

            ScanKeyData metadata_keys[4];
            ScanKeyInit(&metadata_keys[0], k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&metadata_keys[1], k_minimizer_generation_index_query_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&metadata_keys[2], k_minimizer_generation_index_sub_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(k_directory_sub_key)));
            ScanKeyInit(&metadata_keys[3], k_minimizer_generation_index_position_segment,
                BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(-1));
            validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this, &metadata_keys] {
                return systable_beginscan_ordered(
                    relation_, validation_primary_index_,
                    validation_snapshot_, 4, metadata_keys);
            });
            const auto next_validation_tuple = [this] {
                return bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return systable_getnext_ordered(
                        validation_scan_, ForwardScanDirection);
                });
            };
            HeapTuple tuple = next_validation_tuple();
            if (!HeapTupleIsValid(tuple)) {
                minimizer_generation_storage_error("READY manifest row is missing");
            }
            minimizer_generation_loaded_row manifest_row =
                load_minimizer_generation_row_or_error(relation_, tuple);
            inject_minimizer_generation_failure_as_cpp(
                minimizer_generation_failure_injection::generation_reread,
                "generation_reread");
            if (manifest_row.query_key != query_key_
                || manifest_row.address != generation.manifest_row.address
                || manifest_row.checksum
                    != generation.manifest_row.row_checksum
                || !std::ranges::equal(
                    manifest_row.bytes, generation.manifest_row.bytes())) {
                manifest_row.release();
                minimizer_generation_storage_error("READY manifest reread disagrees");
            }
            manifest_row.release();

            crc64_ecma182_accumulator reread_directory_checksum;
            std::uint64_t reread_directory_bytes = 0;
            const std::span<const std::byte> expected_directory =
                generation.directory.encoded_bytes();
            for (std::uint32_t chunk_no = 1;
                 chunk_no <= generation.manifest.directory_chunk_count;
                 ++chunk_no) {
                tuple = next_validation_tuple();
                if (!HeapTupleIsValid(tuple)) {
                    minimizer_generation_storage_error("READY directory chunk is missing");
                }
                minimizer_generation_loaded_row directory_row =
                    load_minimizer_generation_row_or_error(relation_, tuple);
                const std::uint64_t offset64 =
                    static_cast<std::uint64_t>(chunk_no - 1u)
                    * extent_payload_bytes;
                const std::size_t offset = static_cast<std::size_t>(offset64);
                const std::size_t count = std::min<std::size_t>(
                    extent_payload_bytes,
                    expected_directory.size() - offset);
                const auto expected_bytes = expected_directory.subspan(
                    offset, count);
                if (directory_row.query_key != query_key_
                    || directory_row.address != index_chunk_address{
                        .generation_id = generation_id_,
                        .position_segment = -1,
                        .sub_key = k_directory_sub_key,
                        .chunk_no = chunk_no,
                    }
                    || !std::ranges::equal(
                        directory_row.bytes, expected_bytes)) {
                    directory_row.release();
                    minimizer_generation_storage_error("READY directory reread disagrees");
                }
                reread_directory_checksum.update(directory_row.bytes);
                if (!checked_add_(reread_directory_bytes,
                        directory_row.bytes.size())) {
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
            if (reread_directory_bytes
                    != generation.manifest.directory_bytes
                || reread_directory_checksum.checksum()
                    != generation.manifest.directory_checksum) {
                minimizer_generation_storage_error(
                    "READY directory aggregate reread disagrees");
            }

            ScanKeyData payload_keys[3];
            ScanKeyInit(&payload_keys[0], k_minimizer_generation_index_generation_id,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(generation_id_)));
            ScanKeyInit(&payload_keys[1], k_minimizer_generation_index_query_key,
                BTEqualStrategyNumber, F_INT8EQ,
                Int64GetDatum(static_cast<std::int64_t>(query_key_)));
            ScanKeyInit(&payload_keys[2], k_minimizer_generation_index_position_segment,
                BTGreaterEqualStrategyNumber, F_INT4GE, Int32GetDatum(0));
            validation_scan_ = bioseq::postgres::support::invoke_pg_as_cpp(
                [this, &payload_keys] {
                return systable_beginscan_ordered(
                    relation_, validation_primary_index_,
                    validation_snapshot_, 3, payload_keys);
            });
            const auto directory_entries = generation.directory.entries();
            std::size_t entry_ordinal = 0;
            std::uint32_t entry_chunk = 0;
            bool position_extent = false;
            bool payload_complete = directory_entries.empty();
            std::uint64_t reread_payload_rows = 0;
            std::uint64_t reread_payload_bytes = 0;
            crc64_ecma182_accumulator extent_checksum;
            for (tuple = next_validation_tuple();
                 HeapTupleIsValid(tuple);
                 tuple = next_validation_tuple()) {
                if (payload_complete
                    || entry_ordinal >= directory_entries.size()) {
                    minimizer_generation_storage_error("READY payload reread has extra rows");
                }
                const grouped_soa_bucket_directory_entry& entry =
                    directory_entries[entry_ordinal];
                const auto position_sub_key =
                    derive_minimizer_position_extent_sub_key(entry.sub_key);
                if (!position_sub_key) {
                    minimizer_generation_storage_error(
                        "READY directory position sub-key is invalid");
                }
                const std::uint32_t extent_chunk_count = position_extent
                    ? entry.position_chunk_count : entry.control_chunk_count;
                const std::uint64_t extent_bytes = position_extent
                    ? entry.position_encoded_bytes : entry.control_encoded_bytes;
                const std::uint64_t offset =
                    static_cast<std::uint64_t>(entry_chunk)
                    * extent_payload_bytes;
                const std::uint64_t expected_bytes =
                    std::min<std::uint64_t>(
                        extent_payload_bytes,
                        extent_bytes - offset);
                minimizer_generation_loaded_row payload_row =
                    load_minimizer_generation_row_or_error(relation_, tuple);
                if (payload_row.query_key != query_key_
                    || payload_row.address != index_chunk_address{
                        .generation_id = generation_id_,
                        .position_segment = entry.position_segment,
                        .sub_key = position_extent
                            ? *position_sub_key : entry.sub_key,
                        .chunk_no = entry_chunk,
                    }
                    || payload_row.bytes.size() != expected_bytes) {
                    payload_row.release();
                    minimizer_generation_storage_error("READY payload row/address/size disagrees");
                }
                extent_checksum.update(payload_row.bytes);
                if (!checked_add_(reread_payload_rows, 1)
                    || !checked_add_(reread_payload_bytes,
                        payload_row.bytes.size())) {
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
                        position_extent
                        ? entry.position_checksum : entry.control_checksum;
                    if (extent_checksum.checksum()
                        != expected_extent_checksum) {
                        minimizer_generation_storage_error(position_extent
                            ? "READY position extent checksum disagrees"
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
            // The S1 PK is the physical authority: every global control
            // sub-key precedes every derived position sub-key. Wire payload
            // checksums use emitter order, so the sink validates that aggregate
            // before this physical reread.
            if (!payload_complete
                || entry_chunk != 0
                || reread_payload_rows != expected_payload_rows
                || reread_payload_bytes != expected_payload_bytes
                || reread_payload_bytes != generation.manifest.payload_bytes) {
                minimizer_generation_storage_error(
                    "READY complete payload aggregate reread disagrees");
            }
            if (fragment_image_writer) {
                validated_file_receipt = fragment_image_writer->publish(
                    generation.head,
                    generation.manifest,
                    generation.directory,
                    generation.manifest_row.bytes());
            }

            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] {
                    index_close(validation_primary_index_, AccessShareLock);
                });
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
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentTransactionId(); });
        minimizer_generation_administration_identity validated_identity =
            std::move(identity_);
        return ready_minimizer_generation_token{
            std::move(validated_identity),
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

static_assert(bioseq::indexing::minimizer_generation_row_sink<
    index_storage_fragment_generation_row_sink>);
static_assert(bioseq::indexing::grouped_soa_minimizer_payload_sink<
    index_storage_fragment_generation_row_sink>);
static_assert(!std::is_default_constructible_v<
    index_storage_fragment_generation_row_sink>);
static_assert(!std::is_copy_constructible_v<
    index_storage_fragment_generation_row_sink>);
static_assert(std::is_nothrow_move_constructible_v<
    index_storage_fragment_generation_row_sink>);
static_assert(!std::is_move_assignable_v<
    index_storage_fragment_generation_row_sink>);

/// Lock and return the current head bytes used to serialize descriptor publication.
[[nodiscard]] inline std::optional<std::array<
    std::byte, minimizer_generation_head::encoded_bytes>>
read_locked_generation_head_for_descriptor_publication_pg(
        const std::string_view table_name,
        const std::uint64_t query_key) {
    std::array<std::byte, minimizer_generation_head::encoded_bytes> bytes{};
    std::array<Oid, 5u> types{
        INT8OID, INT8OID, INT8OID, INT4OID, INT4OID};
    std::array<Datum, 5u> arguments{
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)),
        Int32GetDatum(-1),
        Int32GetDatum(0),
    };
    const std::string owned_table_name{table_name};
    const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp(
        [&owned_table_name] {
        return quote_identifier(owned_table_name.c_str());
    });
    const std::string sql = fmt::format(
        "SELECT blob FROM bioseq_internal.{} "
        "WHERE query_key=$1 AND generation_id=$2 AND sub_key=$3 "
        "AND position_segment=$4 AND chunk_no=$5 FOR UPDATE",
        quoted_table);
    struct operation_state {
        int connect = 0;
        int execute = 0;
        int finish = 0;
        std::uint64_t rows = 0u;
        bool table = false;
        bool is_null = true;
        bool exact_size = false;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, &sql, &types, &arguments, &bytes] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT) return;
            state.execute = SPI_execute_with_args(
                sql.c_str(), static_cast<int>(types.size()), types.data(),
                arguments.data(), nullptr, false, 1);
            state.rows = SPI_processed;
            state.table = SPI_tuptable != nullptr;
            if (state.execute == SPI_OK_SELECT && state.rows == 1u
                && state.table) {
                const Datum datum = SPI_getbinval(
                    SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                    1, &state.is_null);
                if (!state.is_null) {
                    auto* detoasted = reinterpret_cast<bytea*>(
                        PG_DETOAST_DATUM_PACKED(datum));
                    const std::size_t size = VARSIZE_ANY_EXHDR(detoasted);
                    if (size == bytes.size()) {
                        std::memcpy(
                            bytes.data(), VARDATA_ANY(detoasted), size);
                        state.exact_size = true;
                    }
                    if (static_cast<void*>(detoasted)
                        != DatumGetPointer(datum)) {
                        pfree(detoasted);
                    }
                }
            }
            state.finish = SPI_finish();
        });
    if (state.connect != SPI_OK_CONNECT
        || state.execute != SPI_OK_SELECT || state.rows > 1u
        || (state.rows == 1u
            && (!state.table || state.is_null || !state.exact_size))
        || state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "descriptor publication head lock disagrees: connect={} "
            "execute={} rows={} table={} null={} size={} finish={}",
            state.connect, state.execute, state.rows, state.table,
            state.is_null, state.exact_size, state.finish));
    }
    if (state.rows == 0u) return std::nullopt;
    return bytes;
}

/// Lock and decode the unique LIVE descriptor for one logical generation key.
[[nodiscard]] inline std::optional<minimizer_generation_storage_descriptor>
read_locked_live_descriptor_for_publication_pg(
        const minimizer_generation_administration_identity& identity,
        const std::uint64_t query_key) {
    NameData index_name{};
    bioseq::postgres::support::invoke_pg_as_cpp([&index_name, &identity] {
        namestrcpy(&index_name, identity.index_name.c_str());
    });
    std::array<Oid, 7u> types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID, INT8OID};
    std::array<Datum, 7u> arguments{
        ObjectIdGetDatum(identity.database_oid),
        ObjectIdGetDatum(identity.tablespace_oid),
        ObjectIdGetDatum(identity.main_relfilenode),
        ObjectIdGetDatum(identity.user_relid),
        Int16GetDatum(identity.column_attno),
        NameGetDatum(&index_name),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
    };
    static constexpr std::string_view k_select_sql{
        "SELECT * FROM bioseq_internal.minimizer_generation_storage "
        "WHERE database_oid=$1 AND tablespace_oid=$2 "
        "AND main_relfilenode=$3 AND user_relid=$4 "
        "AND column_attno=$5 AND index_name=$6 AND query_key=$7 "
        "AND state=smallint '1' FOR UPDATE"};
    struct raw_row {
        Oid database_oid = InvalidOid;
        Oid tablespace_oid = InvalidOid;
        Oid main_relfilenode = InvalidOid;
        Oid user_relid = InvalidOid;
        std::int16_t column_attno = 0;
        NameData index_name{};
        std::int64_t query_key = -1;
        std::int64_t generation_id = 0;
        std::int16_t storage_kind = -1;
        Oid payload_relation_oid = InvalidOid;
        Oid payload_database_oid = InvalidOid;
        Oid payload_tablespace_oid = InvalidOid;
        Oid payload_relfilenode = InvalidOid;
        NameData payload_relation_name{};
        std::int64_t image_bytes = 0;
        std::int64_t header_checksum = 0;
        FullTransactionId retire_full_xid = InvalidFullTransactionId;
        std::int16_t state = 0;
        bool payload_relation_oid_null = true;
        bool payload_database_oid_null = true;
        bool payload_tablespace_oid_null = true;
        bool payload_relfilenode_null = true;
        bool payload_relation_name_null = true;
        bool retire_full_xid_null = true;
        bool required_null = false;
    } row{};
    struct operation_state {
        int connect = 0;
        int execute = 0;
        int finish = 0;
        std::uint64_t rows = 0u;
        bool table = false;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, &row, &types, &arguments] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT) return;
            state.execute = SPI_execute_with_args(
                k_select_sql.data(), static_cast<int>(types.size()), types.data(),
                arguments.data(), nullptr, false, 1);
            state.rows = SPI_processed;
            state.table = SPI_tuptable != nullptr;
            if (state.execute == SPI_OK_SELECT && state.rows == 1u
                && state.table) {
                HeapTuple tuple = SPI_tuptable->vals[0];
                TupleDesc descriptor = SPI_tuptable->tupdesc;
                bool is_null = false;
                const auto required =
                    [tuple, descriptor, &is_null, &row](
                            const int attribute) {
                    const Datum value = SPI_getbinval(
                        tuple, descriptor, attribute, &is_null);
                    row.required_null = row.required_null || is_null;
                    return value;
                };
                row.database_oid = DatumGetObjectId(required(1));
                row.tablespace_oid = DatumGetObjectId(required(2));
                row.main_relfilenode = DatumGetObjectId(required(3));
                row.user_relid = DatumGetObjectId(required(4));
                row.column_attno = DatumGetInt16(required(5));
                row.index_name = *DatumGetName(required(6));
                row.query_key = DatumGetInt64(required(7));
                row.generation_id = DatumGetInt64(required(8));
                row.storage_kind = DatumGetInt16(required(9));
                Datum value = SPI_getbinval(
                    tuple, descriptor, 10, &row.payload_relation_oid_null);
                if (!row.payload_relation_oid_null) {
                    row.payload_relation_oid = DatumGetObjectId(value);
                }
                value = SPI_getbinval(
                    tuple, descriptor, 11, &row.payload_database_oid_null);
                if (!row.payload_database_oid_null) {
                    row.payload_database_oid = DatumGetObjectId(value);
                }
                value = SPI_getbinval(
                    tuple, descriptor, 12, &row.payload_tablespace_oid_null);
                if (!row.payload_tablespace_oid_null) {
                    row.payload_tablespace_oid = DatumGetObjectId(value);
                }
                value = SPI_getbinval(
                    tuple, descriptor, 13, &row.payload_relfilenode_null);
                if (!row.payload_relfilenode_null) {
                    row.payload_relfilenode = DatumGetObjectId(value);
                }
                value = SPI_getbinval(
                    tuple, descriptor, 14, &row.payload_relation_name_null);
                if (!row.payload_relation_name_null) {
                    row.payload_relation_name = *DatumGetName(value);
                }
                row.image_bytes = DatumGetInt64(required(15));
                row.header_checksum = DatumGetInt64(required(16));
                value = SPI_getbinval(
                    tuple, descriptor, 17, &row.retire_full_xid_null);
                if (!row.retire_full_xid_null) {
                    row.retire_full_xid = DatumGetFullTransactionId(value);
                }
                row.state = DatumGetInt16(required(18));
            }
            state.finish = SPI_finish();
        });
    if (state.connect != SPI_OK_CONNECT
        || state.execute != SPI_OK_SELECT || state.rows > 1u
        || (state.rows == 1u && (!state.table || row.required_null))
        || state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "descriptor publication LIVE lookup disagrees: connect={} "
            "execute={} rows={} table={} required_null={} finish={}",
            state.connect, state.execute, state.rows, state.table,
            row.required_null, state.finish));
    }
    if (state.rows == 0u) return std::nullopt;
    if (row.query_key < 0 || row.generation_id <= 0
        || row.image_bytes
            < static_cast<std::int64_t>(
                bioseq::indexing::k_minimizer_fragment_image_header_bytes)
        || row.storage_kind
            != static_cast<std::int16_t>(
                bioseq::indexing::
                    k_minimizer_fragment_image_relation_backed_storage_kind)) {
        minimizer_generation_storage_error_as_cpp(
            "descriptor publication LIVE row scalar shape is invalid");
    }
    minimizer_generation_storage_descriptor result{
        .database_oid = row.database_oid,
        .tablespace_oid = row.tablespace_oid,
        .main_relfilenode = row.main_relfilenode,
        .user_relid = row.user_relid,
        .column_attno = row.column_attno,
        .index_name = NameStr(row.index_name),
        .query_key = static_cast<std::uint64_t>(row.query_key),
        .generation_id = static_cast<std::uint64_t>(row.generation_id),
        .payload_relation_oid = row.payload_relation_oid_null
            ? std::nullopt
            : std::optional<Oid>{row.payload_relation_oid},
        .payload_database_oid = row.payload_database_oid_null
            ? std::nullopt
            : std::optional<Oid>{row.payload_database_oid},
        .payload_tablespace_oid = row.payload_tablespace_oid_null
            ? std::nullopt
            : std::optional<Oid>{row.payload_tablespace_oid},
        .payload_relfilenode = row.payload_relfilenode_null
            ? std::nullopt
            : std::optional<Oid>{row.payload_relfilenode},
        .payload_relation_name = row.payload_relation_name_null
            ? std::nullopt
            : std::optional<std::string>{NameStr(
                row.payload_relation_name)},
        .image_bytes = static_cast<std::uint64_t>(row.image_bytes),
        .header_checksum = checksum_from_pg_bigint(row.header_checksum),
        .retire_full_xid = row.retire_full_xid_null
            ? std::nullopt
            : std::optional<std::uint64_t>{U64FromFullTransactionId(
                row.retire_full_xid)},
        .state = static_cast<minimizer_generation_storage_state>(row.state),
    };
    if (!exact_storage_descriptor_identity(result, identity, query_key)
        || !has_exact_storage_descriptor_relation_shape(result)
        || result.state != minimizer_generation_storage_state::live
        || result.retire_full_xid) {
        minimizer_generation_storage_error_as_cpp(
            "descriptor publication LIVE row shape disagrees");
    }
    return result;
}

/// Retire the previous descriptor and install the new LIVE descriptor atomically.
inline void apply_minimizer_generation_storage_transition_pg(
        const minimizer_generation_administration_identity& identity,
        const std::uint64_t query_key,
        const std::optional<minimizer_generation_storage_descriptor>& old_live,
        const std::optional<minimizer_generation_storage_descriptor>& new_live) {
    if (!old_live && !new_live) return;
    if ((old_live && (!exact_storage_descriptor_identity(
                *old_live, identity, query_key)
            || old_live->state != minimizer_generation_storage_state::live
            || old_live->retire_full_xid))
        || (new_live && (!exact_storage_descriptor_identity(
                *new_live, identity, query_key)
            || new_live->state != minimizer_generation_storage_state::live
            || new_live->retire_full_xid
            || !has_exact_storage_descriptor_relation_shape(*new_live)
            || !bioseq::indexing::sql_bigint_can_represent(
                new_live->generation_id)
            || !bioseq::indexing::sql_bigint_can_represent(
                new_live->image_bytes)))) {
        minimizer_generation_storage_error_as_cpp(
            "generation storage transition binding is invalid");
    }

    NameData index_name{};
    NameData payload_relation_name{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&index_name, &payload_relation_name, &identity, &new_live] {
        namestrcpy(&index_name, identity.index_name.c_str());
        if (new_live && new_live->payload_relation_name) {
            namestrcpy(&payload_relation_name,
                new_live->payload_relation_name->c_str());
        }
    });
    std::array<Oid, 9u> update_types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID,
        INT8OID, INT8OID, XID8OID};
    std::array<Datum, 9u> update_arguments{
        ObjectIdGetDatum(identity.database_oid),
        ObjectIdGetDatum(identity.tablespace_oid),
        ObjectIdGetDatum(identity.main_relfilenode),
        ObjectIdGetDatum(identity.user_relid),
        Int16GetDatum(identity.column_attno),
        NameGetDatum(&index_name),
        Int64GetDatum(static_cast<std::int64_t>(query_key)),
        Int64GetDatum(old_live
            ? static_cast<std::int64_t>(old_live->generation_id) : 0),
        old_live
            ? FullTransactionIdGetDatum(
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetTopFullTransactionId(); }))
            : FullTransactionIdGetDatum(InvalidFullTransactionId),
    };
    static constexpr std::string_view k_update_sql{
        "UPDATE bioseq_internal.minimizer_generation_storage "
        "SET retire_full_xid=$9, state=smallint '2' "
        "WHERE database_oid=$1 AND tablespace_oid=$2 "
        "AND main_relfilenode=$3 AND user_relid=$4 "
        "AND column_attno=$5 AND index_name=$6 AND query_key=$7 "
        "AND generation_id=$8 AND state=smallint '1' "
        "AND retire_full_xid IS NULL"};

    std::array<Oid, k_storage_descriptor_attribute_count> insert_types{
        OIDOID, OIDOID, OIDOID, OIDOID, INT2OID, NAMEOID,
        INT8OID, INT8OID, INT2OID,
        OIDOID, OIDOID, OIDOID, OIDOID, NAMEOID,
        INT8OID, INT8OID, XID8OID, INT2OID};
    std::array<Datum, k_storage_descriptor_attribute_count> insert_arguments{};
    std::array<char, k_storage_descriptor_attribute_count> insert_nulls{};
    if (new_live) {
        insert_arguments = {
            ObjectIdGetDatum(new_live->database_oid),
            ObjectIdGetDatum(new_live->tablespace_oid),
            ObjectIdGetDatum(new_live->main_relfilenode),
            ObjectIdGetDatum(new_live->user_relid),
            Int16GetDatum(new_live->column_attno),
            NameGetDatum(&index_name),
            Int64GetDatum(static_cast<std::int64_t>(new_live->query_key)),
            Int64GetDatum(static_cast<std::int64_t>(
                new_live->generation_id)),
            Int16GetDatum(static_cast<std::int16_t>(
                bioseq::indexing::
                    k_minimizer_fragment_image_relation_backed_storage_kind)),
            new_live->payload_relation_oid
                ? ObjectIdGetDatum(*new_live->payload_relation_oid) : 0,
            new_live->payload_database_oid
                ? ObjectIdGetDatum(*new_live->payload_database_oid) : 0,
            new_live->payload_tablespace_oid
                ? ObjectIdGetDatum(*new_live->payload_tablespace_oid) : 0,
            new_live->payload_relfilenode
                ? ObjectIdGetDatum(*new_live->payload_relfilenode) : 0,
            new_live->payload_relation_name
                ? NameGetDatum(&payload_relation_name) : 0,
            Int64GetDatum(static_cast<std::int64_t>(new_live->image_bytes)),
            Int64GetDatum(checksum_to_pg_bigint(
                new_live->header_checksum)),
            0,
            Int16GetDatum(static_cast<std::int16_t>(
                minimizer_generation_storage_state::live)),
        };
        insert_nulls.fill(' ');
        for (const std::size_t attribute : {9u, 10u, 11u, 12u, 13u}) {
            const bool present = attribute == 9u
                ? new_live->payload_relation_oid.has_value()
                : attribute == 10u
                    ? new_live->payload_database_oid.has_value()
                    : attribute == 11u
                        ? new_live->payload_tablespace_oid.has_value()
                        : attribute == 12u
                            ? new_live->payload_relfilenode.has_value()
                            : new_live->payload_relation_name.has_value();
            if (!present) insert_nulls[attribute] = 'n';
        }
        insert_nulls[16] = 'n';
    }
    static constexpr std::string_view k_insert_sql{
        "INSERT INTO bioseq_internal.minimizer_generation_storage "
        "(database_oid,tablespace_oid,main_relfilenode,user_relid,"
        "column_attno,index_name,query_key,generation_id,provider,"
        "payload_relation_oid,payload_database_oid,payload_tablespace_oid,"
        "payload_relfilenode,payload_relation_name,image_bytes,"
        "header_checksum,retire_full_xid,state) VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,"
        "$17,$18)"};
    struct operation_state {
        int connect = 0;
        int update = 0;
        int insert = 0;
        int finish = 0;
        std::uint64_t updated = 0u;
        std::uint64_t inserted = 0u;
    } state{};
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&state, &update_types, &update_arguments, &insert_types,
         &insert_arguments, &insert_nulls, &old_live,
         &new_live] noexcept {
            state.connect = SPI_connect();
            if (state.connect != SPI_OK_CONNECT) return;
            if (old_live) {
                state.update = SPI_execute_with_args(
                    k_update_sql.data(), static_cast<int>(update_types.size()),
                    update_types.data(), update_arguments.data(), nullptr,
                    false, 0);
                state.updated = SPI_processed;
                if (state.update != SPI_OK_UPDATE || state.updated != 1u) {
                    state.finish = SPI_finish();
                    return;
                }
            }
            if (new_live) {
                state.insert = SPI_execute_with_args(
                    k_insert_sql.data(), static_cast<int>(insert_types.size()),
                    insert_types.data(), insert_arguments.data(),
                    insert_nulls.data(), false, 0);
                state.inserted = SPI_processed;
            }
            state.finish = SPI_finish();
        });
    if (state.connect != SPI_OK_CONNECT
        || (old_live
            && (state.update != SPI_OK_UPDATE || state.updated != 1u))
        || (new_live
            && (state.insert != SPI_OK_INSERT || state.inserted != 1u))
        || state.finish != SPI_OK_FINISH) {
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation storage transition disagrees: connect={} "
            "update={} updated={} insert={} inserted={} finish={}",
            state.connect, state.update, state.updated, state.insert,
            state.inserted, state.finish));
    }
}

/// Consume the private complete-reread authorization and publish its head atomically.
/// There is no public overload that accepts an arbitrary relation/generation.
[[nodiscard]] inline ready_minimizer_generation_published_head_receipt
publish_ready_minimizer_generation_head_pg(
        ready_minimizer_generation_token&& token) {
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct publication_state {
        Relation relation = nullptr;
        Relation descriptor_relation = nullptr;
        bytea* blob = nullptr;
        std::string sql;
    };
    auto* publication = new publication_state{};
    std::array<std::byte, minimizer_generation_head::encoded_bytes>
        returned_head_bytes{};
    std::array<std::byte, minimizer_generation_head::encoded_bytes>
        previous_head_bytes{};
    bool returned_valid = false;
    bool previous_present = false;
    std::optional<minimizer_generation_head> decoded_previous_head{};

    try {
        if (!token.valid_
            || !bioseq::indexing::sql_bigint_can_represent(token.query_key_)
            || token.generation_id_ == 0
            || token.identity_.database_oid != MyDatabaseId
            || !TransactionIdIsValid(token.transaction_id_)
            || token.transaction_id_
                != bioseq::postgres::support::invoke_pg_as_cpp(
                    [] { return GetCurrentTransactionIdIfAny(); })
            || token.advisory_lock_key_
                != index_advisory_xact_lock_key(
                    token.identity_.database_oid,
                    token.identity_.tablespace_oid,
                    token.identity_.main_relfilenode,
                    token.identity_.column_attno,
                    token.identity_.index_name)) {
            minimizer_generation_storage_error("READY publication token is invalid");
        }
        bioseq::postgres::support::invoke_pg_as_cpp([&token] {
            acquire_index_advisory_xact_lock(
                token.identity_.database_oid,
                token.identity_.tablespace_oid,
                token.identity_.main_relfilenode,
                token.identity_.column_attno,
                token.identity_.index_name);
        });
        publication->relation = bioseq::postgres::support::invoke_pg_as_cpp(
            [publication, &token] {
            return bioseq::postgres::support::try_open_postgres_relation(
                token.sidecar_relation_oid_, RowExclusiveLock);
        });
        if (publication->relation == nullptr
            || RelationGetNamespace(publication->relation)
                != bioseq_internal_ns()
            || std::string_view{
                RelationGetRelationName(publication->relation)}
                != token.identity_.table_name
            || publication->relation->rd_locator.spcOid
                != token.sidecar_locator_.spcOid
            || publication->relation->rd_locator.dbOid
                != token.sidecar_locator_.dbOid
            || publication->relation->rd_locator.relNumber
                != token.sidecar_locator_.relNumber) {
            minimizer_generation_storage_error(
                "READY publication relation identity changed after reread");
        }
        const minimizer_generation_schema_inspection schema =
            bioseq::postgres::support::invoke_pg_as_cpp([publication, &token] {
                return inspect_minimizer_generation_schema(
                    publication->relation, nullptr,
                    minimizer_generation_schema_expectation{
                        .namespace_oid = bioseq_internal_ns(),
                        .relation_name = token.identity_.table_name,
                        .tablespace_oid = token.identity_.tablespace_oid,
                    });
            });
        if (!schema) {
            minimizer_generation_storage_error("READY publication schema changed after reread");
        }
        auto decoded_token_head =
            bioseq::indexing::decode_minimizer_generation_head(
                token.head_bytes_);
        if (!decoded_token_head
            || decoded_token_head->generation_id != token.generation_id_
            || decoded_token_head->source_identity != token.source_identity_
            || decoded_token_head->source_content_checksum
                != token.source_content_checksum_
            || decoded_token_head->storage_contract_checksum
                != token.storage_contract_checksum_
            || token.head_row_checksum_
                != bioseq::indexing::crc64_index_chunk_row(
                    0, -1, k_generation_head_sub_key, 0,
                    static_cast<std::uint32_t>(token.head_bytes_.size()),
                    token.head_bytes_)) {
            minimizer_generation_storage_error("READY publication token head disagrees");
        }

        const auto locked_previous_head =
            read_locked_generation_head_for_descriptor_publication_pg(
                token.identity_.table_name, token.query_key_);
        previous_present = locked_previous_head.has_value();
        if (locked_previous_head) {
            previous_head_bytes = *locked_previous_head;
            auto decoded_previous_head_result =
                bioseq::indexing::decode_minimizer_generation_head(
                    previous_head_bytes);
            if (!decoded_previous_head_result
                || decoded_previous_head_result->generation_id == 0u
                || decoded_previous_head_result->generation_id
                    == token.generation_id_) {
                minimizer_generation_storage_error(
                    "READY publication previous head is malformed or names the new generation");
            }
            decoded_previous_head = *decoded_previous_head_result;
        }

        const bool fragment_index = token.identity_.index_name
            == k_minimizers_per_fragment_index_name;
        const bool new_grouped_soa = decoded_token_head->layout
            == bioseq::indexing::minimizer_generation_layout::
                grouped_soa_segment_local;
        if (!fragment_index && token.file_receipt_) {
            minimizer_generation_storage_error(
                "non-fragment generation carried a minimizer fragment image file receipt");
        }
        if (fragment_index) {
            const Oid descriptor_oid =
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    return get_relname_relid(
                        "minimizer_generation_storage",
                        bioseq_internal_ns());
                });
            if (!OidIsValid(descriptor_oid)) {
                minimizer_generation_storage_error(
                    "generation storage descriptor catalog is missing");
            }
            publication->descriptor_relation =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [publication, descriptor_oid] {
                    return bioseq::postgres::support::try_open_postgres_relation(
                        descriptor_oid, RowExclusiveLock);
                });
            if (publication->descriptor_relation == nullptr
                || RelationGetNamespace(publication->descriptor_relation)
                    != bioseq_internal_ns()
                || std::string_view{RelationGetRelationName(
                    publication->descriptor_relation)}
                    != "minimizer_generation_storage") {
                minimizer_generation_storage_error(
                    "generation storage descriptor catalog identity changed");
            }
            bioseq::storage::
                validate_minimizer_generation_storage_catalogs_pg();

            auto old_live = read_locked_live_descriptor_for_publication_pg(
                token.identity_, token.query_key_);
            const bool old_grouped_soa = decoded_previous_head
                && decoded_previous_head->layout
                    == bioseq::indexing::minimizer_generation_layout::
                        grouped_soa_segment_local;
            if (old_grouped_soa != old_live.has_value()
                || (old_live
                    && old_live->generation_id
                        != decoded_previous_head->generation_id)) {
                minimizer_generation_storage_error(
                    "active head and LIVE generation descriptor disagree");
            }
            if (old_live
                && (old_live->payload_relation_oid
                        != std::optional<Oid>{token.sidecar_relation_oid_}
                    || old_live->payload_database_oid
                        != std::optional<Oid>{token.sidecar_locator_.dbOid}
                    || old_live->payload_tablespace_oid
                        != std::optional<Oid>{token.sidecar_locator_.spcOid}
                    || old_live->payload_relfilenode
                        != std::optional<Oid>{
                            static_cast<Oid>(
                                token.sidecar_locator_.relNumber)}
                    || old_live->payload_relation_name
                        != std::optional<std::string>{
                            token.identity_.table_name})) {
                minimizer_generation_storage_error(
                    "previous relation-backed descriptor locator changed");
            }
            if (new_grouped_soa != token.file_receipt_.has_value()) {
                minimizer_generation_storage_error(
                    "new minimizer-fragment grouped-SoA head and file receipt disagree");
            }
            std::optional<minimizer_generation_storage_descriptor> new_live;
            if (token.file_receipt_) {
                const minimizer_fragment_image_file_receipt& file = *token.file_receipt_;
                if (file.file_bytes
                        < bioseq::indexing::k_minimizer_fragment_image_header_bytes
                    || !bioseq::indexing::sql_bigint_can_represent(
                        file.file_bytes)) {
                    minimizer_generation_storage_error(
                        "new minimizer fragment image file receipt cannot be cataloged");
                }
                new_live = minimizer_generation_storage_descriptor{
                    .database_oid = token.identity_.database_oid,
                    .tablespace_oid = token.identity_.tablespace_oid,
                    .main_relfilenode = token.identity_.main_relfilenode,
                    .user_relid = token.identity_.user_relid,
                    .column_attno = token.identity_.column_attno,
                    .index_name = token.identity_.index_name,
                    .query_key = token.query_key_,
                    .generation_id = token.generation_id_,
                    .payload_relation_oid =
                        std::optional<Oid>{token.sidecar_relation_oid_},
                    .payload_database_oid =
                        std::optional<Oid>{token.sidecar_locator_.dbOid},
                    .payload_tablespace_oid =
                        std::optional<Oid>{token.sidecar_locator_.spcOid},
                    .payload_relfilenode = std::optional<Oid>{
                        static_cast<Oid>(token.sidecar_locator_.relNumber)},
                    .payload_relation_name = std::optional<std::string>{
                        token.identity_.table_name},
                    .image_bytes = file.file_bytes,
                    .header_checksum = file.header_crc64,
                    .retire_full_xid = std::nullopt,
                    .state = minimizer_generation_storage_state::live,
                };
            }
            apply_minimizer_generation_storage_transition_pg(
                token.identity_, token.query_key_, old_live, new_live);
        }

        publication->blob = bioseq::postgres::support::invoke_pg_as_cpp(
            [publication, &token] {
            return reinterpret_cast<bytea*>(
                palloc(VARHDRSZ + token.head_bytes_.size()));
        });
        SET_VARSIZE(publication->blob, VARHDRSZ + token.head_bytes_.size());
        std::memcpy(VARDATA(publication->blob), token.head_bytes_.data(),
            token.head_bytes_.size());
    std::array<Oid, 8> argument_types{
        INT8OID, INT8OID, INT4OID, INT8OID,
        INT4OID, INT4OID, INT8OID, BYTEAOID,
    };
    std::array<Datum, 8> arguments{
        Int64GetDatum(static_cast<std::int64_t>(token.query_key_)),
        Int64GetDatum(0),
        Int32GetDatum(-1),
        Int64GetDatum(static_cast<std::int64_t>(k_generation_head_sub_key)),
        Int32GetDatum(0),
        Int32GetDatum(static_cast<std::int32_t>(token.head_bytes_.size())),
        Int64GetDatum(checksum_to_pg_bigint(token.head_row_checksum_)),
        PointerGetDatum(publication->blob),
    };
    const char* quoted_table = bioseq::postgres::support::invoke_pg_as_cpp(
        [&token] {
        return quote_identifier(token.identity_.table_name.c_str());
    });
    publication->sql = fmt::format(
        "INSERT INTO bioseq_internal.{} "
        "(query_key,generation_id,position_segment,sub_key,chunk_no,"
        "chunk_bytes,checksum,blob) VALUES "
        "($1,$2,$3,$4,$5,$6,$7,$8) "
        "ON CONFLICT "
        "(generation_id,query_key,sub_key,position_segment,chunk_no) "
        "DO UPDATE SET blob=EXCLUDED.blob, "
        "chunk_bytes=EXCLUDED.chunk_bytes, checksum=EXCLUDED.checksum "
        "RETURNING blob",
        quoted_table);
        struct publish_spi_state {
            int connect_status = 0;
            int execute_status = 0;
            int finish_status = 0;
            std::uint64_t processed = 0;
            bool table_present = false;
            bool returned_is_null = true;
            bool returned_size_valid = false;
        } spi{};
        const char* publication_sql = publication->sql.c_str();
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&spi, publication_sql, &argument_types, &arguments,
             &returned_head_bytes] noexcept {
                spi.connect_status = SPI_connect();
                if (spi.connect_status != SPI_OK_CONNECT) return;
                spi.execute_status = SPI_execute_with_args(
                    publication_sql,
                    static_cast<int>(argument_types.size()),
                    argument_types.data(), arguments.data(),
                    nullptr, false, 1);
                inject_minimizer_generation_failure_leaf(
                    minimizer_generation_failure_injection::generation_spi_publish,
                    "generation_spi_publish");
                spi.processed = SPI_processed;
                spi.table_present = SPI_tuptable != nullptr;
                if (spi.execute_status == SPI_OK_INSERT_RETURNING
                    && spi.processed == 1 && spi.table_present) {
                    const Datum returned_blob = SPI_getbinval(
                        SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
                        1, &spi.returned_is_null);
                    if (!spi.returned_is_null) {
                        auto* detoasted = reinterpret_cast<bytea*>(
                            PG_DETOAST_DATUM_PACKED(returned_blob));
                        const std::size_t bytes =
                            VARSIZE_ANY_EXHDR(detoasted);
                        if (bytes == returned_head_bytes.size()) {
                            std::memcpy(
                                returned_head_bytes.data(),
                                VARDATA_ANY(detoasted), bytes);
                            spi.returned_size_valid = true;
                        }
                        if (static_cast<void*>(detoasted)
                            != DatumGetPointer(returned_blob)) {
                            pfree(detoasted);
                        }
                    }
                }
                spi.finish_status = SPI_finish();
            });
        returned_valid = spi.returned_size_valid;
        if (spi.connect_status != SPI_OK_CONNECT
            || spi.execute_status != SPI_OK_INSERT_RETURNING
            || spi.processed != 1 || !spi.table_present
            || spi.returned_is_null || !spi.returned_size_valid
            || spi.finish_status != SPI_OK_FINISH) {
            minimizer_generation_storage_error(fmt::format(
                "READY publication SPI session disagrees: connect={} "
                "execute={} rows={} table={} null={} size={} finish={}",
                spi.connect_status, spi.execute_status, spi.processed,
                spi.table_present, spi.returned_is_null,
                spi.returned_size_valid, spi.finish_status));
        }
        bytea* finished_blob = std::exchange(publication->blob, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_blob] { pfree(finished_blob); });
        if (publication->descriptor_relation != nullptr) {
            Relation finished_descriptor = std::exchange(
                publication->descriptor_relation, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp([finished_descriptor] {
                relation_close(finished_descriptor, RowExclusiveLock);
            });
        }
        Relation finished_relation = std::exchange(
            publication->relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_relation] {
                relation_close(finished_relation, RowExclusiveLock);
            });
        auto returned_head =
            bioseq::indexing::decode_minimizer_generation_head(
                returned_head_bytes);
        if (!returned_valid || !returned_head
            || !bioseq::indexing::is_fixed_wire_returned_exactly(
                token.head_bytes_, returned_head_bytes)
            || *returned_head != *decoded_token_head) {
            minimizer_generation_storage_error(
                "READY publication did not return the byte-exact validated head");
        }
        std::optional<std::uint64_t> replaced_generation_id;
        if (previous_present) {
            const auto previous_head =
                bioseq::indexing::decode_minimizer_generation_head(
                    previous_head_bytes);
            if (!previous_head || previous_head->generation_id == 0
                || previous_head->generation_id
                    == returned_head->generation_id) {
                minimizer_generation_storage_error(
                    "READY publication previous head is malformed or names the new generation");
            }
            replaced_generation_id = previous_head->generation_id;
        }
        ready_minimizer_generation_published_head_receipt receipt{
            .table_name = token.identity_.table_name,
            .query_key = token.query_key_,
            .head = *returned_head,
            .head_bytes = returned_head_bytes,
            .replaced_generation_id = replaced_generation_id,
        };
        token.invalidate();
        std::string{}.swap(publication->sql);
        delete publication;
        return receipt;
    } catch (const bioseq::postgres::support::postgres_error&) {
        // The current transaction/ResourceOwner owns SPI, relation and palloc
        // state. Do not issue PostgreSQL cleanup calls while propagating the
        // exact original error.
        publication->blob = nullptr;
        publication->descriptor_relation = nullptr;
        publication->relation = nullptr;
        token.invalidate();
        delete publication;
        throw;
    } catch (...) {
        std::exception_ptr failure = std::current_exception();
        publication->blob = nullptr;
        publication->descriptor_relation = nullptr;
        publication->relation = nullptr;
        token.invalidate();
        delete publication;
        std::rethrow_exception(failure);
    }
    delete publication;
    pg_unreachable();
}


}  // namespace bioseq::storage::indexing
