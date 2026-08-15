/**
 * @file hash_major.cppm
 * @brief Hash-major minimizer generation encoding and publication.
 */
module;
#include <libassert/assert.hpp>
#include <bioseq/pg_compat.h>
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "commands/tablespace.h"
#include "executor/tuptable.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/memutils.h"
extern "C" {
#include "storage/buffile.h"
}
#include <algorithm>
#include <array>
#include <concepts>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <expected>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
export module bioseq.postgres.storage.indexing.admin:hash_major;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.manager;                     // seq_caches, sequence_locator, is_locator_stub, decode_locator_from_stub
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.lazy_sequence;                // make_resident_lazy_sequence
import bioseq.storage.scanner;             // sequence access requirements
import bioseq.postgres.storage.scanner;          // with_postgres_lazy_sequence
import bioseq.postgres.types.compact_sequence;   // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                   // compact_sequence_range
import bioseq.common.seqan_adapter;                      // seqan3::char_is_valid_for
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.registry;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_sequence;
import bioseq.pipeline.runtime_contract;   // minimizer_algorithm
import bioseq.sequences.kmers;          // minimizer/index APIs
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.dispatch_contract;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.residency;
#ifdef BIOSEQ_TEST_INDEXES
import bioseq.postgres.storage.indexing.sorted_runs;
#endif
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.postgres.table.io;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;                     // column_table_row_count
static_assert(__cplusplus >= 202302L, "C++23-or-later");
import :policy_catalog;

export namespace bioseq::storage::indexing {

// ---- SQL helper implementations ------------------------------------------

/// Read-only setup and post-measurement inspection returned by the SQL admin
/// path. The authoritative source locator is derived from the visible
/// Bioseq row. Callers never supply a source identity or storage-contract
/// digest.
struct ready_minimizer_generation_inspection {
    ready_minimizer_generation_identity identity{};
    minimizer_generation_source_locator source{};
    minimizer_generation_manifest manifest{};
    std::uint64_t source_content_checksum = 0;
    Oid database_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
};

/// One fully authenticated hash-major logical-stream receipt.  The descriptor
/// fields are publication authority. The exact_catalog_* fields are recomputed
/// by a bounded, row-aligned reread under the same registered MVCC snapshot.
struct hash_major_minimizer_generation_stream_inspection {
    bioseq::indexing::hash_major_minimizer_stream_kind kind =
        bioseq::indexing::hash_major_minimizer_stream_kind::directory;
    std::uint64_t element_count = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint64_t stream_crc64 = 0;
    std::uint64_t exact_catalog_rows = 0;
    std::uint64_t exact_catalog_bytes = 0;
    std::uint64_t exact_catalog_stream_crc64 = 0;
};

/// Separate hash-major restart/audit surface.  This must not be folded into the
/// grouped-SoA inspector: the two sentinels have different identity widths,
/// metadata sizes, and stream equations.
struct hash_major_minimizer_generation_inspection {
    Oid database_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint64_t query_key = 0;
    std::array<char, NAMEDATALEN> table_name{};
    std::array<char, NAMEDATALEN> index_name{};
    minimizer_generation_source_locator source{};
    std::uint64_t source_content_crc64 = 0;
    std::array<std::byte, 16> source_identity{};
    bioseq::indexing::hash_major_minimizer_head head{};
    bioseq::indexing::hash_major_minimizer_manifest manifest{};
    bioseq::indexing::encoded_hash_major_minimizer_head encoded_head{};
    bioseq::indexing::encoded_hash_major_minimizer_manifest encoded_manifest{};
    std::uint64_t head_rows = 0;
    std::uint64_t head_bytes = 0;
    std::uint64_t manifest_rows = 0;
    std::uint64_t manifest_bytes = 0;
    std::array<hash_major_minimizer_generation_stream_inspection, 3> streams{};
};

/// Resolve the sole live sequence column required by inspection helpers.
[[nodiscard]] inline std::int16_t
single_seq_attno_for_inspection(Oid rel_oid);

/// The execution-receipt alternative is the tag. There is deliberately no independent
/// mutable index-kind enum that a caller could relabel after an encoder returns.
using ready_minimizer_generation_execution_receipt = std::variant<
    std::monostate,
    bioseq::indexing::minimizer_fragment_generation_execution_receipt,
    bioseq::indexing::minimizer_sequence_generation_execution_receipt>;

/// Physical grouped-SoA producer family authenticated by a publication.
enum class ready_minimizer_generation_index_kind : std::uint8_t {
    fragment,
    sequence,
};

/// Validated result of the production minimizer generation coordinator.
/// Metadata age is deliberately absent: callers own that update and may perform
/// it only after this function returns a published result.
class ready_minimizer_generation_publication {
private:
    template <class p_sequence_range>
    friend ready_minimizer_generation_publication
    build_and_publish_grouped_soa_minimizer_generation_pg(
        Oid,
        std::int16_t,
        std::string_view,
        std::uint64_t,
        fragment_index_storage&,
        bioseq::storage::lazy_sequence_view<
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type>,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>,
        bioseq::minimizers::minimizer_sketch_backend,
        const bioseq::minimizers::minimizer_kernels*);

    ready_minimizer_generation_index_kind index_kind_ =
        ready_minimizer_generation_index_kind::fragment;
    bioseq::indexing::minimizer_generation_publication_binding binding_{};
    std::uint64_t control_count_ = 0;
    std::uint64_t position_count_ = 0;
    std::uint64_t writer_rows_ = 0;
    std::uint64_t writer_batches_ = 0;
    bioseq::minimizers::minimizer_sketch_backend selected_backend_ =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    ready_minimizer_generation_execution_receipt execution_{};
    std::optional<std::uint64_t> replaced_generation_id_{};
    std::uint64_t minimizer_sequence_spill_generation_token_ = 0;
    std::uint64_t minimizer_sequence_spill_mutation_before_release_ = 0;
    std::uint64_t minimizer_sequence_spill_mutation_after_release_ = 0;
    bool minimizer_sequence_spill_resources_released_ = false;
    bool published_ = false;

    ready_minimizer_generation_publication(
            ready_minimizer_generation_index_kind index_kind,
            bioseq::indexing::minimizer_generation_publication_binding binding,
            std::uint64_t control_count,
            std::uint64_t position_count,
            std::uint64_t writer_rows,
            std::uint64_t writer_batches,
            bioseq::minimizers::minimizer_sketch_backend backend,
            ready_minimizer_generation_execution_receipt execution,
            std::optional<std::uint64_t> replaced_generation_id,
            std::uint64_t minimizer_sequence_spill_generation_token,
            std::uint64_t minimizer_sequence_spill_mutation_before_release,
            std::uint64_t minimizer_sequence_spill_mutation_after_release,
            bool minimizer_sequence_spill_resources_released) noexcept
        : index_kind_(index_kind),
          binding_(binding),
          control_count_(control_count),
          position_count_(position_count),
          writer_rows_(writer_rows),
          writer_batches_(writer_batches),
          selected_backend_(backend),
          execution_(std::move(execution)),
          replaced_generation_id_(replaced_generation_id),
          minimizer_sequence_spill_generation_token_(
              minimizer_sequence_spill_generation_token),
          minimizer_sequence_spill_mutation_before_release_(
              minimizer_sequence_spill_mutation_before_release),
          minimizer_sequence_spill_mutation_after_release_(
              minimizer_sequence_spill_mutation_after_release),
          minimizer_sequence_spill_resources_released_(
              minimizer_sequence_spill_resources_released),
          published_(true) {}

public:
    ready_minimizer_generation_publication() = delete;
    ready_minimizer_generation_publication(
        const ready_minimizer_generation_publication&) = default;
    ready_minimizer_generation_publication(
        ready_minimizer_generation_publication&&) noexcept = default;
    ready_minimizer_generation_publication& operator=(
        const ready_minimizer_generation_publication&) = delete;
    ready_minimizer_generation_publication& operator=(
        ready_minimizer_generation_publication&&) noexcept = delete;

    [[nodiscard]] ready_minimizer_generation_index_kind
    index_kind() const noexcept { return index_kind_; }
    [[nodiscard]] std::uint64_t generation_id() const noexcept {
        return binding_.generation_id;
    }
    [[nodiscard]] std::uint64_t source_identity() const noexcept {
        return binding_.source_identity;
    }
    [[nodiscard]] std::uint64_t source_content_checksum() const noexcept {
        return binding_.source_content_checksum;
    }
    [[nodiscard]] std::uint64_t storage_contract_checksum() const noexcept {
        return binding_.storage_contract_checksum;
    }
    [[nodiscard]] std::uint64_t control_count() const noexcept {
        return control_count_;
    }
    [[nodiscard]] std::uint64_t position_count() const noexcept {
        return position_count_;
    }
    [[nodiscard]] std::uint64_t writer_rows() const noexcept {
        return writer_rows_;
    }
    [[nodiscard]] std::uint64_t writer_batches() const noexcept {
        return writer_batches_;
    }
    [[nodiscard]] bioseq::minimizers::minimizer_sketch_backend
    selected_backend() const noexcept { return selected_backend_; }
    [[nodiscard]] const ready_minimizer_generation_execution_receipt&
    execution() const noexcept { return execution_; }
    [[nodiscard]] std::optional<std::uint64_t>
    replaced_generation_id() const noexcept {
        return replaced_generation_id_;
    }
    [[nodiscard]] std::uint64_t
    minimizer_sequence_spill_generation_token() const noexcept {
        return minimizer_sequence_spill_generation_token_;
    }
    [[nodiscard]] std::uint64_t
    minimizer_sequence_spill_mutation_before_release() const noexcept {
        return minimizer_sequence_spill_mutation_before_release_;
    }
    [[nodiscard]] std::uint64_t
    minimizer_sequence_spill_mutation_after_release() const noexcept {
        return minimizer_sequence_spill_mutation_after_release_;
    }
    [[nodiscard]] bool minimizer_sequence_spill_resources_released() const noexcept {
        return minimizer_sequence_spill_resources_released_;
    }
    [[nodiscard]] bool published() const noexcept { return published_; }

    /// Verify that the producer receipt matches every sealed publication field.
    [[nodiscard]] bool execution_accepted() const noexcept {
        if (!published_ || !binding_.valid()) {
            return false;
        }
        if (const auto* fragment_execution = std::get_if<
                bioseq::indexing::minimizer_fragment_generation_execution_receipt>(
                    &execution_)) {
            return index_kind_ == ready_minimizer_generation_index_kind::fragment
                && !minimizer_sequence_spill_resources_released_
                && minimizer_sequence_spill_generation_token_ == 0
                && minimizer_sequence_spill_mutation_before_release_ == 0
                && minimizer_sequence_spill_mutation_after_release_ == 0
                && fragment_execution->selected_backend == selected_backend_
                && fragment_execution->publication_accepted(binding_);
        }
        if (const auto* sequence_execution = std::get_if<
                bioseq::indexing::minimizer_sequence_generation_execution_receipt>(
                    &execution_)) {
            return index_kind_ == ready_minimizer_generation_index_kind::sequence
                && minimizer_sequence_spill_resources_released_
                && minimizer_sequence_spill_generation_token_ != 0
                && minimizer_sequence_spill_mutation_before_release_
                    != (std::numeric_limits<std::uint64_t>::max)()
                && minimizer_sequence_spill_mutation_after_release_
                    == minimizer_sequence_spill_mutation_before_release_ + 1u
                && sequence_execution->selected_backend == selected_backend_
                && sequence_execution->matches_publication(binding_)
                && sequence_execution->replay_final_storage_binding.generation_token
                    == minimizer_sequence_spill_generation_token_
                && sequence_execution->replay_final_storage_binding.mutation_revision
                    == minimizer_sequence_spill_mutation_before_release_;
        }
        return false;
    }
};

static_assert(!std::is_aggregate_v<ready_minimizer_generation_publication>);
static_assert(!std::is_default_constructible_v<
    ready_minimizer_generation_publication>);
static_assert(!std::is_copy_assignable_v<
    ready_minimizer_generation_publication>);
static_assert(!std::is_move_assignable_v<
    ready_minimizer_generation_publication>);
/// Detect forbidden public mutation of authenticated publication fields.
template <class p_publication>
concept publicly_relabelable_ready_minimizer_publication = requires(
        p_publication& publication,
        ready_minimizer_generation_execution_receipt execution) {
    publication.execution = execution;
    publication.generation_id = std::uint64_t{1};
    publication.source_identity = std::uint64_t{1};
    publication.selected_backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    publication.published = true;
};
static_assert(!publicly_relabelable_ready_minimizer_publication<
    ready_minimizer_generation_publication>);

/// Raise one coordinator diagnostic through a leaf PostgreSQL boundary. The
/// caller receives postgres_error only after PG_END_TRY restored the PG
/// exception stack, so no encoder, run-store, authorization, or writer object is
/// skipped by longjmp.
[[noreturn]] inline void ready_minimizer_error_as_cpp(
        int sqlstate,
        std::string_view message,
        std::string_view detail = {}) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, message, detail] {
        if (detail.empty()) {
            ereport(ERROR,
                (errcode(sqlstate),
                 errmsg("%.*s", static_cast<int>(message.size()),
                    message.data())));
        }
        ereport(ERROR,
            (errcode(sqlstate),
             errmsg("%.*s", static_cast<int>(message.size()),
                message.data()),
             errdetail("%.*s", static_cast<int>(detail.size()),
                detail.data())));
    });
    pg_unreachable();
}

/// One bounded first-pass record.  The file is PostgreSQL-owned temporary
/// scratch and is never a publication artifact, so native words are sufficient:
/// it is rewound and consumed inside this statement by the same backend.
struct hash_major_minimizer_count_record {
    std::uint64_t hash = 0;
    std::uint64_t exact_count = 0;
};
static_assert(sizeof(hash_major_minimizer_count_record) == 16u);

/// PG BufFile scratch for hash-major's count-before-postings contract.  A first
/// replay writes one fixed record per distinct hash. The second replay reads
/// records in lockstep while postings stream directly into the logical encoder.
/// PostgreSQL's ResourceOwner owns a file after any ERROR.  Success closes it
/// explicitly. Exception paths only abandon the C++ handle.
class hash_major_minimizer_count_scratch {
private:
    static constexpr std::size_t k_records_per_io = 4'096u;

    BufFile* file_ = nullptr;
    std::array<hash_major_minimizer_count_record, k_records_per_io> buffer_{};
    std::uint64_t record_count_ = 0u;
    std::uint64_t records_read_ = 0u;
    std::size_t buffered_records_ = 0u;
    std::size_t next_buffered_record_ = 0u;
    bool reading_ = false;

    void flush_write_buffer_() {
        if (buffered_records_ == 0u) return;
        const std::size_t bytes =
            buffered_records_ * sizeof(hash_major_minimizer_count_record);
        bioseq::postgres::support::invoke_pg_as_cpp([this, bytes] {
            BufFileWrite(file_, buffer_.data(), bytes);
        });
        buffered_records_ = 0u;
    }

public:
    /// Create PostgreSQL-owned temporary storage for first-pass counts.
    hash_major_minimizer_count_scratch() {
        file_ = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return BufFileCreateTemp(false); });
        if (file_ == nullptr) {
            ready_minimizer_error_as_cpp(
                ERRCODE_OUT_OF_MEMORY,
                "hash-major hash-count scratch could not be created");
        }
    }

    hash_major_minimizer_count_scratch(
        const hash_major_minimizer_count_scratch&) = delete;
    hash_major_minimizer_count_scratch& operator=(
        const hash_major_minimizer_count_scratch&) = delete;
    hash_major_minimizer_count_scratch(
        hash_major_minimizer_count_scratch&&) = delete;
    hash_major_minimizer_count_scratch& operator=(
        hash_major_minimizer_count_scratch&&) = delete;
    ~hash_major_minimizer_count_scratch() = default;

    /// Buffer one nonempty globally ordered hash count.
    void append(const hash_major_minimizer_count_record& record) {
        if (file_ == nullptr || reading_ || records_read_ != 0u
            || record.exact_count == 0u
            || record_count_
                == (std::numeric_limits<std::uint64_t>::max)()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch append state is invalid");
        }
        buffer_[buffered_records_++] = record;
        if (buffered_records_ == buffer_.size()) {
            flush_write_buffer_();
        }
        ++record_count_;
    }

    /// Flush and rewind the scratch file for one complete sequential replay.
    void rewind_for_read() {
        if (file_ == nullptr || reading_ || records_read_ != 0u) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch rewind state is invalid");
        }
        flush_write_buffer_();
        if (bioseq::postgres::support::invoke_pg_as_cpp([this] {
                    return BufFileSeek(file_, 0, 0, SEEK_SET);
                }) != 0) {
            ready_minimizer_error_as_cpp(
                ERRCODE_IO_ERROR,
                "hash-major hash-count scratch rewind failed");
        }
        reading_ = true;
        buffered_records_ = 0u;
        next_buffered_record_ = 0u;
    }

    /// Read the next count record or report completion.
    [[nodiscard]] std::optional<hash_major_minimizer_count_record>
    read_next() {
        if (file_ == nullptr || !reading_
            || records_read_ > record_count_) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch read state is invalid");
        }
        if (records_read_ == record_count_) {
            return std::nullopt;
        }
        if (next_buffered_record_ == buffered_records_) {
            const std::uint64_t remaining = record_count_ - records_read_;
            buffered_records_ = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer_.size()));
            next_buffered_record_ = 0u;
            const std::size_t bytes = buffered_records_
                * sizeof(hash_major_minimizer_count_record);
            bioseq::postgres::support::invoke_pg_as_cpp([this, bytes] {
                BufFileReadExact(file_, buffer_.data(), bytes);
            });
        }
        const hash_major_minimizer_count_record record =
            buffer_[next_buffered_record_++];
        ++records_read_;
        if (record.exact_count == 0u) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch contains an empty hash");
        }
        return record;
    }

    [[nodiscard]] std::uint64_t record_count() const noexcept {
        return record_count_;
    }

    [[nodiscard]] std::uint64_t records_read() const noexcept {
        return records_read_;
    }

    /// Close only after every count record has been consumed.
    void close_checked() {
        if (file_ != nullptr
            && (!reading_ || records_read_ != record_count_)) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major hash-count scratch closed before complete replay");
        }
        BufFile* closing = std::exchange(file_, nullptr);
        if (closing != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [closing] { BufFileClose(closing); });
        }
    }

    /// Abandon the local handle after PostgreSQL assumes file cleanup.
    void abandon_after_pg_error_noexcept() noexcept {
        file_ = nullptr;
    }
};

/// Validate and count one globally sorted replay without retaining postings.
/// The single target-wide segment used by the producer is what makes hash
/// ordering global instead of restarting at every persisted position segment.
struct hash_major_minimizer_count_replay {
    hash_major_minimizer_count_scratch* scratch = nullptr;
    std::uint64_t target_length = 0u;
    std::uint64_t maximum_hash = 0u;
    std::uint64_t current_hash = 0u;
    std::uint64_t current_count = 0u;
    std::uint64_t previous_position = 0u;
    std::uint64_t event_count = 0u;
    bool have_hash = false;

    /// Persist the completed count for the current hash.
    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    flush_hash() {
        if (!have_hash) return {};
        if (scratch == nullptr || current_count == 0u) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::row_count);
        }
        scratch->append(hash_major_minimizer_count_record{
            .hash = current_hash,
            .exact_count = current_count,
        });
        current_count = 0u;
        return {};
    }

    /// Validate and count one canonical event from the first replay.
    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    accept(const bioseq::indexing::canonical_minimizer_event& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        if (scratch == nullptr || target_length == 0u
            || event.hash > maximum_hash || position >= target_length) {
            return std::unexpected(
                event.hash > maximum_hash
                    ? bioseq::indexing::minimizer_generation_encoding_error::hash_range
                    : bioseq::indexing::minimizer_generation_encoding_error::
                        position_range);
        }
        if (have_hash) {
            if (event.hash < current_hash
                || (event.hash == current_hash
                    && event.encoded_position <= previous_position)) {
                return std::unexpected(
                    event.hash == current_hash
                        && event.encoded_position == previous_position
                    ? bioseq::indexing::minimizer_generation_encoding_error::
                        duplicate_event
                    : bioseq::indexing::minimizer_generation_encoding_error::
                        row_order);
            }
            if (event.hash != current_hash) {
                if (auto flushed = flush_hash(); !flushed) return flushed;
                current_hash = event.hash;
            }
        } else {
            current_hash = event.hash;
            have_hash = true;
        }
        if (current_count
                == (std::numeric_limits<std::uint64_t>::max)()
            || event_count
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    arithmetic_overflow);
        }
        ++current_count;
        ++event_count;
        previous_position = event.encoded_position;
        return {};
    }

    /// Flush the final hash count after the first replay.
    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    finish() {
        return flush_hash();
    }
};

/// Consume the second identical replay in lockstep with first-pass count
/// records.  Only the current 16-byte record and encoder-owned row buffers are
/// live, even when one hash owns billions of postings.
struct hash_major_minimizer_encode_replay {
    hash_major_minimizer_count_scratch* scratch = nullptr;
    bioseq::indexing::hash_major_minimizer_logical_encoder* encoder = nullptr;
    std::optional<bioseq::indexing::hash_major_minimizer_error>* encoding_error =
        nullptr;
    std::uint64_t target_length = 0u;
    std::uint64_t maximum_hash = 0u;
    std::uint64_t current_hash = 0u;
    std::uint64_t previous_position = 0u;
    std::uint64_t event_count = 0u;
    bool hash_open = false;

    /// Preserve a logical-encoder failure in the owning publication state.
    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    report_encoding_error(bioseq::indexing::hash_major_minimizer_error error) {
        if (encoding_error != nullptr) {
            *encoding_error = error;
        }
        return std::unexpected(
            bioseq::indexing::minimizer_generation_encoding_error::
                storage_contract_mismatch);
    }

    /// Finish the current hash in the logical encoder.
    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    close_hash() {
        if (!hash_open) return {};
        if (auto ended = encoder->end_hash(); !ended) {
            return report_encoding_error(ended.error());
        }
        hash_open = false;
        return {};
    }

    /// Open one encoder hash using its exact first-pass count.
    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    open_hash(std::uint64_t hash) {
        const auto record = scratch->read_next();
        if (!record || record->hash != hash) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    replay_mismatch);
        }
        if (auto begun = encoder->begin_hash(
                record->hash, record->exact_count); !begun) {
            return report_encoding_error(begun.error());
        }
        current_hash = hash;
        hash_open = true;
        return {};
    }

    /// Validate and encode one canonical event from the second replay.
    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    accept(const bioseq::indexing::canonical_minimizer_event& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        if (scratch == nullptr || encoder == nullptr || target_length == 0u
            || event.hash > maximum_hash || position >= target_length) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    replay_mismatch);
        }
        if (!hash_open || event.hash != current_hash) {
            if (hash_open && event.hash < current_hash) {
                return std::unexpected(
                    bioseq::indexing::minimizer_generation_encoding_error::
                        replay_mismatch);
            }
            if (auto closed = close_hash(); !closed) return closed;
            if (auto opened = open_hash(event.hash); !opened) return opened;
        } else if (event.encoded_position <= previous_position) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    replay_mismatch);
        }
        if (auto appended = encoder->append_posting(
                event.encoded_position); !appended) {
            return report_encoding_error(appended.error());
        }
        if (event_count
                == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    arithmetic_overflow);
        }
        ++event_count;
        previous_position = event.encoded_position;
        return {};
    }

    /// Finish the final hash and require complete count-record consumption.
    [[nodiscard]] std::expected<
        void, bioseq::indexing::minimizer_generation_encoding_error>
    finish() {
        if (auto closed = close_hash(); !closed) return closed;
        if (scratch == nullptr
            || scratch->records_read() != scratch->record_count()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::
                    replay_mismatch);
        }
        return {};
    }
};

/// Minimal validated result for the production hash-major publication. The
/// private storage token has already enforced complete payload/manifest reread
/// before this value can exist, and the published head is byte-compared below.
struct ready_hash_major_minimizer_publication {
    std::uint64_t generation_id = 0u;
    std::uint64_t query_key = 0u;
    std::uint64_t source_epoch = 0u;
    std::uint64_t source_content_crc64 = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t total_posting_count = 0u;
    bioseq::minimizers::minimizer_sketch_backend selected_backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    bioseq::indexing::hash_major_minimizer_head head{};
    bioseq::indexing::hash_major_minimizer_manifest manifest{};
    std::optional<std::uint64_t> replaced_generation_id{};
    bool published = false;
};

/// Build and atomically publish one sequence-wide hash-major minimizer generation
/// from a fragmented PostgreSQL source. Native minimizers are collected into
/// PostgreSQL-owned sorted spill runs. Two bounded replays then satisfy the
/// encoder's exact-count-before-postings contract. Fragment indexes are
/// intentionally excluded: their physical indexes must never cross this merge seam.
template <class p_sequence_range>
[[nodiscard]] inline ready_hash_major_minimizer_publication
build_and_publish_hash_major_minimizer_generation_pg(
        Oid user_relid,
        std::int16_t column_attno,
        std::string_view index_name,
        std::uint64_t sequence_rowno,
        fragment_index_storage& storage,
        bioseq::storage::lazy_sequence_view<
            typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::
                alphabet_type> sequence,
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding,
        bioseq::minimizers::minimizer_sketch_backend backend,
        const bioseq::minimizers::minimizer_kernels* kernels) {
    using source_binding_type =
        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>;
    using backend_type =
        bioseq::minimizers::minimizer_sketch_backend;
    using sequence_index =
        bioseq::indexing::direct_minimizer_bucket_index<fragment_index_storage>;
    require_selected_minimizer_kernels(
        backend, kernels, "hash-major minimizer generation");
    using traits_type = typename compact_sequence_range<p_sequence_range>::traits;
    using canonical_sequence_config = sequence_custom_alphabet<
        typename traits_type::alphabet_type,
        typename traits_type::storage_unit>;
    static_assert(traits_type::bits_per_char
        == compact_sequence_range<canonical_sequence_config>::traits::
            bits_per_char,
        "hash-major publisher requires canonical PostgreSQL packing");
    const sequence_locator& locator = source_binding.locator();
    Snapshot registered_source_snapshot =
        source_binding.registered_snapshot();

    const bool is_sequence_index =
        index_name == k_minimizers_per_sequence_index_name;
    const bool active_snapshot_ready =
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            return ActiveSnapshotSet()
                && IsMVCCSnapshot(GetActiveSnapshot());
        });
    if (!OidIsValid(user_relid) || column_attno <= 0 || !is_sequence_index
        || source_binding.column_attno() != column_attno
        || !bioseq::indexing::sql_bigint_can_represent(sequence_rowno)
        || locator.payload_version != sequence_locator_payload_version || locator.source_epoch == 0u
        || locator.logical_length == 0u || locator.fragment_count == 0u
        || registered_source_snapshot == nullptr
        || !IsMVCCSnapshot(registered_source_snapshot)
        || registered_source_snapshot->regd_count == 0
        || !active_snapshot_ready) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "hash-major minimizer generation coordinator input is invalid");
    }
    if (backend != backend_type::scalar && backend != backend_type::exact_simd
        && backend != backend_type::bounded_nthash_simd
        && backend != backend_type::vulkan_segment) {
        ready_minimizer_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "hash-major minimizer generation backend is invalid");
    }
    constexpr std::uint16_t k_window_size = 10u;
    constexpr std::uint16_t k_kmer_size = 15u;
    if (static_cast<std::uint64_t>(sequence.bp_size())
            != locator.logical_length) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "hash-major minimizer lazy sequence shape disagrees with its locator");
    }

    const relation_identity relation_identity =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [user_relid] { return index_relation_identity(user_relid); });
    if (relation_identity.db_oid != MyDatabaseId
        || !OidIsValid(relation_identity.tablespace_oid)
        || !OidIsValid(relation_identity.relfilenode)
        || storage.db_oid_ != relation_identity.db_oid
        || storage.tablespace_oid_ != relation_identity.tablespace_oid
        || storage.main_relfilenode_ != relation_identity.relfilenode
        || storage.user_relid_ != user_relid
        || storage.column_attno_ != column_attno
        || storage.index_name_ != index_name
        || storage.bucket_idx_ != locator.bucket_index
        || storage.shard_idx_ != locator.shard_index
        || storage.query_key_ != sequence_rowno
        || source_binding.db_oid() != relation_identity.db_oid
        || source_binding.tablespace_oid()
            != relation_identity.tablespace_oid
        || source_binding.main_relfilenode()
            != relation_identity.relfilenode) {
        ready_minimizer_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "hash-major minimizer storage identity disagrees with its source");
    }

    const bioseq::indexing::minimizer_scheme_identity scheme =
        bioseq::indexing::minimizer_scheme_identity_for_backend(backend);

    struct build_state {
        minimizer_generation_administration_identity identity{};
        std::optional<minimizer_generation_source_authorization> source_authorization;
        minimizer_generation_source_locator source{};
        std::uint64_t source_content_checksum = 0u;
        std::optional<index_storage_hash_major_minimizer_writer> writer;
        std::optional<hash_major_minimizer_count_scratch> count_scratch;
        std::optional<ready_hash_major_minimizer_token> ready_token;
        std::optional<bioseq::indexing::hash_major_minimizer_error> encoding_error;
        bioseq::indexing::minimizer_sequence_generation_execution_receipt execution{};
    };
    auto* state = new build_state{
        .identity = minimizer_generation_administration_identity{
            .database_oid = relation_identity.db_oid,
            .tablespace_oid = relation_identity.tablespace_oid,
            .main_relfilenode = relation_identity.relfilenode,
            .user_relid = user_relid,
            .column_attno = column_attno,
            .index_name = std::string{index_name},
            .table_name = storage.this_table_name(),
        },
    };
    ready_hash_major_minimizer_publication result{
        .query_key = sequence_rowno,
        .source_epoch = locator.source_epoch,
        .selected_backend = backend,
    };

    try {
        scoped_forced_bounded_nthash_index_failure forced_bounded_failure;
        state->source_authorization.emplace(
            authorize_minimizer_generation_source_pg(
                state->identity, sequence_rowno,
                registered_source_snapshot));
        state->source =
            state->source_authorization->source_for_diagnostics();
        state->source_content_checksum = state->source_authorization
            ->source_content_checksum_for_diagnostics();
        if (state->source.sequence_rowno != sequence_rowno
            || !same_minimizer_generation_sequence_locator(state->source.locator, locator)
            || state->source.alphabet_code
                != source_binding_type::alphabet_code()
            || state->source.storage_unit_byte_width
                != source_binding_type::storage_unit_byte_width()
            || state->source.padding_letters
                != source_binding.last_fragment_padding()
            || state->source.locator.logical_length != sequence.bp_size()
            || state->source.locator.fragment_count != locator.fragment_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major authoritative source authorization disagrees "
                "with the passed lazy sequence");
        }
        result.source_content_crc64 = state->source_content_checksum;

        const auto derived_profile = bioseq::indexing::
            hash_major_minimizer_profile_for_backend(
                backend,
                state->source.alphabet_code,
                k_kmer_size,
                k_window_size,
                locator.logical_length);
        if (!derived_profile) {
            ready_minimizer_error_as_cpp(
                ERRCODE_FEATURE_NOT_SUPPORTED,
                "source/backend combination has no canonical hash-major profile",
                fmt::format(
                    "backend={} alphabet_code={} k={} w={} encoding_error={}",
                    bioseq::minimizers::
                        minimizer_sketch_backend_name(backend),
                    state->source.alphabet_code,
                    k_kmer_size, k_window_size,
                    static_cast<unsigned>(derived_profile.error())));
        }
        const bioseq::indexing::hash_major_minimizer_storage_profile profile =
            *derived_profile;

        ensure_vulkan_index_build_runtime_ready(backend);
        result.generation_id = bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return allocate_minimizer_generation_id_pg(); });
        state->writer.emplace(storage.begin_hash_major_minimizer_generation_write(
            sequence_rowno, result.generation_id));
        state->count_scratch.emplace();
        forced_bounded_failure.arm_after_publisher_state_begin(backend);

        const bioseq::indexing::hash_major_minimizer_generation_options options{
            .generation_id = result.generation_id,
            .query_key = sequence_rowno,
            .source_epoch = locator.source_epoch,
            .source_content_crc64 = state->source_content_checksum,
            .target_length = locator.logical_length,
            .storage_profile = profile,
        };
        const std::uint64_t maximum_hash =
            bioseq::indexing::hash_major_minimizer_max_hash(profile);

        auto spill = sequence_index::spill_generation_replay_runs_serial(
            storage,
            sequence,
            backend,
            kernels,
            /*core_span_bp=*/0u,
            /*position_segment_span_bp=*/locator.logical_length,
            &state->execution,
            state->source.alphabet_code);
        if (!spill) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major minimizer spill producer failed",
                fmt::format(
                    "backend={} wire_error={}",
                    bioseq::minimizers::
                        minimizer_sketch_backend_name(backend),
                    static_cast<unsigned>(spill.error())));
        }

        std::optional<bioseq::indexing::hash_major_minimizer_encoded_metadata>
            encoded_metadata;
        {
            auto producer = sequence_index::make_generation_replay_producer(
                storage,
                locator.logical_length,
                std::move(*spill),
                locator.logical_length);

            hash_major_minimizer_count_replay count_pass{
                .scratch = &*state->count_scratch,
                .target_length = locator.logical_length,
                .maximum_hash = maximum_hash,
            };
            auto counted = producer.scan_sorted_events(
                locator.logical_length,
                bioseq::indexing::canonical_minimizer_event_receiver{
                    .context = &count_pass,
                    .emit_fn = +[](
                            void* context,
                            const bioseq::indexing::canonical_minimizer_event&
                                event)
                        -> std::expected<
                            void,
                            bioseq::indexing::minimizer_generation_encoding_error> {
                        return static_cast<hash_major_minimizer_count_replay*>(
                            context)->accept(event);
                    },
                });
            if (counted) counted = count_pass.finish();
            if (!counted) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "hash-major count replay failed",
                    fmt::format(
                        "wire_error={}",
                        static_cast<unsigned>(counted.error())));
            }
            state->count_scratch->rewind_for_read();

            bioseq::indexing::hash_major_minimizer_logical_encoder encoder{
                options,
                state->writer->logical_stream_sink(),
            };
            hash_major_minimizer_encode_replay encode_pass{
                .scratch = &*state->count_scratch,
                .encoder = &encoder,
                .encoding_error = &state->encoding_error,
                .target_length = locator.logical_length,
                .maximum_hash = maximum_hash,
            };
            auto emitted = producer.scan_sorted_events(
                locator.logical_length,
                bioseq::indexing::canonical_minimizer_event_receiver{
                    .context = &encode_pass,
                    .emit_fn = +[](
                            void* context,
                            const bioseq::indexing::canonical_minimizer_event&
                                event)
                        -> std::expected<
                            void,
                            bioseq::indexing::minimizer_generation_encoding_error> {
                        return static_cast<hash_major_minimizer_encode_replay*>(
                            context)->accept(event);
                    },
                });
            if (emitted) emitted = encode_pass.finish();
            if (!emitted
                || count_pass.event_count != encode_pass.event_count) {
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "hash-major posting replay failed",
                    fmt::format(
                        "wire_error={} encoding_error={} counted={} emitted={}",
                        emitted
                            ? 0u
                            : static_cast<unsigned>(emitted.error()) + 1u,
                        state->encoding_error
                            ? static_cast<unsigned>(*state->encoding_error) + 1u
                            : 0u,
                        count_pass.event_count,
                        encode_pass.event_count));
            }
            auto finished = encoder.finish();
            if (!finished) {
                state->encoding_error = finished.error();
                ready_minimizer_error_as_cpp(
                    ERRCODE_DATA_CORRUPTED,
                    "hash-major logical encoder failed",
                    fmt::format(
                        "encoding_error={}",
                        static_cast<unsigned>(finished.error())));
            }
            encoded_metadata.emplace(std::move(*finished));
            state->execution = producer.execution_receipt();
        }

        const auto expected_source_identity =
            bioseq::indexing::hash_major_minimizer_source_identity(
                sequence_rowno,
                locator.source_epoch,
                state->source_content_checksum,
                locator.logical_length);
        const auto expected_storage_profile_digest =
            bioseq::indexing::hash_major_minimizer_storage_profile_digest(
                profile);
        const auto& metadata = *encoded_metadata;
        if (metadata.head.generation_id != result.generation_id
            || metadata.head.query_key != sequence_rowno
            || metadata.head.source_epoch != locator.source_epoch
            || metadata.head.source_identity != expected_source_identity
            || metadata.head.source_content_crc64
                != state->source_content_checksum
            || metadata.head.storage_profile_digest
                != expected_storage_profile_digest
            || metadata.head.dense_seal.has_value()
            || metadata.manifest.generation_id != result.generation_id
            || metadata.manifest.source_epoch != locator.source_epoch
            || metadata.manifest.source_identity != expected_source_identity
            || metadata.manifest.source_content_crc64
                != state->source_content_checksum
            || metadata.manifest.target_length != locator.logical_length
            || metadata.manifest.storage_profile != profile
            || metadata.manifest.storage_profile_digest
                != expected_storage_profile_digest
            || metadata.manifest.unique_hash_count
                != state->count_scratch->record_count()
            || metadata.manifest.total_posting_count
                != state->execution.spilled_event_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major encoded metadata identity disagrees");
        }

        const auto before_release = storage.segment_spill_storage_binding();
        const std::uint64_t expected_visited =
            state->execution.spilled_event_count
                    <= (std::numeric_limits<std::uint64_t>::max)() / 2u
                ? state->execution.spilled_event_count * 2u
                : (std::numeric_limits<std::uint64_t>::max)();
        if (!state->execution.is_spill_valid()
            || state->execution.selected_backend != backend
            || state->execution.minimizer_scheme != scheme
            || state->execution.target_length != locator.logical_length
            || state->execution.window_size != k_window_size
            || state->execution.kmer_size != k_kmer_size
            || state->execution.generation_layout
                != bioseq::indexing::minimizer_generation_layout::invalid
            || state->execution.replay_scan_count != 2u
            || state->execution.replay_merge_pass_count != 2u
            || state->execution.replay_reroute_pass_count != 0u
            || state->execution.replay_completed_scan_count != 2u
            || state->execution.replay_failed_scan_count != 0u
            || state->execution.replay_binding_checked_failed_scan_count != 0u
            || state->execution.replay_illegal_span_transition_count != 0u
            || state->execution.replay_first_completed_event_count
                != state->execution.spilled_event_count
            || state->execution.replay_last_completed_event_count
                != state->execution.spilled_event_count
            || state->execution.replay_first_completed_content_digest
                != state->execution.spill_content_digest
            || state->execution.replay_last_completed_content_digest
                != state->execution.spill_content_digest
            || state->execution.replay_final_span_bp
                != locator.logical_length
            || state->execution.replay_final_storage_binding
                != before_release
            || state->execution.replay_visited_event_count != expected_visited
            || !before_release.valid()) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major selected backend replay validation was rejected");
        }

        storage.release_runs();
        const auto after_release = storage.segment_spill_storage_binding();
        if (after_release.generation_token != before_release.generation_token
            || before_release.mutation_revision
                == (std::numeric_limits<std::uint64_t>::max)()
            || after_release.mutation_revision
                != before_release.mutation_revision + 1u) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major spill resources did not release atomically");
        }
        state->count_scratch->close_checked();
        state->count_scratch.reset();

        result.unique_hash_count = metadata.manifest.unique_hash_count;
        result.total_posting_count =
            metadata.manifest.total_posting_count;
        result.head = metadata.head;
        result.manifest = metadata.manifest;
        state->ready_token.emplace(
            state->writer->seal_and_validate_ready(
                metadata, std::move(*state->source_authorization)));
        state->source_authorization.reset();
        state->writer.reset();
        storage.close_checked();
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::generation_storage_close,
            "hash_major_minimizer_generation_storage_close");

        ready_hash_major_minimizer_published_head_receipt publication_receipt =
            publish_ready_hash_major_minimizer_head_pg(
                std::move(*state->ready_token));
        state->ready_token.reset();
        if (publication_receipt.query_key != sequence_rowno
            || publication_receipt.head != metadata.head
            || publication_receipt.head_bytes != metadata.encoded_head) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "hash-major publication did not return the validated byte-exact head");
        }
        result.replaced_generation_id =
            publication_receipt.replaced_generation_id;
        result.published = true;
        delete state;
        state = nullptr;
    } catch (const bioseq::postgres::support::postgres_error&) {
        storage.abandon_after_pg_error_noexcept();
        if (state->count_scratch) {
            state->count_scratch->abandon_after_pg_error_noexcept();
        }
        state->ready_token.reset();
        state->writer.reset();
        state->source_authorization.reset();
        delete state;
        state = nullptr;
        throw;
    } catch (...) {
        storage.abandon_after_pg_error_noexcept();
        if (state->count_scratch) {
            state->count_scratch->abandon_after_pg_error_noexcept();
        }
        state->ready_token.reset();
        state->writer.reset();
        state->source_authorization.reset();
        delete state;
        state = nullptr;
        throw;
    }
    return result;
}


}  // namespace bioseq::storage::indexing
