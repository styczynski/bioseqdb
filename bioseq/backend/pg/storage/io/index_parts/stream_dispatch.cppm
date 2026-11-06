/**
 * @file stream_dispatch.cppm
 * @brief Choose the sequence reader and stream its records into PostgreSQL storage.
 */
module;
// Include Highway  and PostgreSQL headers before the named module declaration.
// Imported sequence modules also depend on these declarations.
#include <bioseq/pg_compat.h>
#include <boost/container/small_vector.hpp>
#include <boost/integer.hpp>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <limits>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
extern "C" {
#include "postgres.h"

#include "access/relation.h"     // index_open / index_close
#include "access/table.h"        // table_open / table_close
#include "access/xact.h"         // CommandCounterIncrement
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"   // TEXTOID, VARCHAROID, BPCHAROID
#include "commands/sequence.h"   // ResetSequenceCaches
#include "executor/executor.h"   // ExecConstraints, ExecInitExpr, ExecEvalExpr
#include "executor/tuptable.h"   // TupleTableSlot, MakeTupleTableSlot, TTSOpsVirtual, Exec*Tuple
#include "nodes/makefuncs.h"     // makeNode (ResultRelInfo)
#include "optimizer/optimizer.h" // expression_planner
#include "utils/array.h"         // deconstruct_array, ARR_ELEMTYPE
#include "utils/builtins.h"      // text_to_cstring, cstring_to_text_with_len, regtypein
#include "utils/lsyscache.h"     // getTypeInputInfo, get_typtype, getBaseType
#include "utils/rel.h"           // RelationGetDescr
#include "utils/resowner.h"      // CurrentResourceOwner, TopTransactionResourceOwner
#include "utils/snapmgr.h" // Snapshot stack operations.
#include "utils/typcache.h" // lookup_rowtype_tupdesc

#include "fmgr.h"
#include "rewrite/rewriteHandler.h" // build_column_default
}

export module bioseq.postgres.storage.io:stream_dispatch;

import bioseq.io.sinks;

import bioseq.common.logging; // Load progress logiing
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.configuration.core;
import bioseq.storage.size; // sequence_size_t
import bioseq.storage.layout;
import bioseq.common.templating.runtime_dispatch; // dispatch_exact_value_or_default
import bioseq.common.templating.value_sequence;
import bioseq.sequences;
import bioseq.sequences.simd;                       // Sequence alphabets and packed storage types.

import bioseq.postgres.types.compact_sequence; // PostgreSQL sequence values and supported alphabets.
import bioseq.postgres.storage.manager; // Sequence locators, fragment writers and relation caches.


import bioseq.postgres.table.io; // Column table row writes and per-row memory contexts.
import bioseq.io.options;                      // sequence_file_format, reader_options
import bioseq.io.file_kind;                    // detect_sequence_file_kind, sequence_file_kind
import bioseq.io.errors;
import bioseq.io.reader;                       // Stream sequence chunks and sample input headers.

import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser; // Map input header fields to destination columns.
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;

import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import :typed_row_writer;
import :record_ingestion;
import bioseq.io.record_schema;
import bioseq.io.identifier_parser.preservation;

// Load records one packed chunk at a time.

/** Keep parser classifications plain until the PostgreSQL execution thread reports them. */
[[noreturn]] void raise_load_reader_failure(bioseq::io::input_failure_kind kind,
        const std::string& message, const load_columns& columns) {
    if (kind == bioseq::io::input_failure_kind::invalid_quality_byte) {
        const auto alphabet = pgseq_supported_alphabets::name_of_alphabet_code(columns.quality_alphabet_code);
        raise_load_error_as_cpp(ERRCODE_INVALID_TEXT_REPRESENTATION,
            fmt::format("{} [{}]", message, alphabet));
    }
    raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, message);
}

/** One quality codec axis, independent of the biological parser templates. */
struct load_quality_encoder {
    virtual ~load_quality_encoder() = default;
    virtual void reset() = 0;
    virtual void append(std::span<const std::uint8_t>, std::size_t, std::string_view,
        bioseq::storage::sequence_stream_writer&) = 0;
    virtual std::uint16_t finish(std::uint64_t, bioseq::storage::sequence_stream_writer&) = 0;
};

template<class Alphabet, std::size_t Width>
struct typed_load_quality_encoder final : load_quality_encoder {
    static constexpr auto alphabet_name = pgseq_supported_alphabets::template name_of<Alphabet>();
    using unit = typename boost::uint_t<Width * 8>::least;
    using traits = typename compact_sequence_range<sequence_custom_alphabet<Alphabet, unit>>::traits;
    std::vector<unit> words;
    bioseq::simd::active_target::encode_state<unit, traits::bits_per_char, traits::letters_per_unit> state;
    void reset() override { words.clear(); state.output_storage = &words; state.reset(); }
    void append(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view header,
            bioseq::storage::sequence_stream_writer& writer) override {
        const auto encoded = bioseq::simd::active_target::encode_body_chunk<Alphabet,
            bioseq::simd::active_target::unknown_byte_policy::reject>(state, bytes.data(), bytes.size());
        if (!encoded) {
            raise_load_error_as_cpp(ERRCODE_INVALID_TEXT_REPRESENTATION,
                fmt::format("invalid {} quality byte {} at quality offset {} in record '{}'{}",
                    alphabet_name.view(), encoded.offending_byte,
                    offset + encoded.consumed_bytes, header.substr(0u, 256u),
                    std::is_same_v<Alphabet, seqan3::phred42>
                        ? "; select phred63 explicitly for scores above phred42" : ""));
        }
        if (!words.empty()) writer.append_packed_bytes(
            {reinterpret_cast<const std::uint8_t*>(words.data()), words.size() * sizeof(unit)},
            words.size() * traits::letters_per_unit);
        words.clear();
    }
    std::uint16_t finish(std::uint64_t expected, bioseq::storage::sequence_stream_writer& writer) override {
        if (state.total_letters != expected)
            raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "sequence and quality lengths disagree");
        const auto remainder = state.partial_letters;
        const auto [units, padding] = bioseq::simd::active_target::finalize_encode_state<unit,
            traits::bits_per_char, traits::letters_per_unit>(state);
        if (!words.empty()) writer.append_packed_bytes(
            {reinterpret_cast<const std::uint8_t*>(words.data()), words.size() * sizeof(unit)}, remainder);
        words.clear();
        return static_cast<std::uint16_t>(padding);
    }
};

[[nodiscard]] std::unique_ptr<load_quality_encoder> make_load_quality_encoder(const load_columns& columns) {
    std::unique_ptr<load_quality_encoder> result;
    if (!columns.quality_attno) return result;
    inject_load_failure(7);
    pgseq_supported_quality_alphabets::dispatch_alphabet_by_code(columns.quality_alphabet_code,
        [&result, &columns]<class Alphabet>() {
            pgseq_supported_storage_unit_byte_widths::dispatch(columns.quality_byte_width,
                [&result]<std::size_t Width>() { result = std::make_unique<typed_load_quality_encoder<Alphabet, Width>>(); });
        });
    return result;
}

/** @brief Write sequence chunks as fragments, then store each record's parent row. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
template <sequence_alphabet p_alphabet, std::size_t p_column_byte_width> struct load_stream_sink {
    /// Alphabet and packed unit selected by the  target column.
    using col_cfg =
        sequence_custom_alphabet<p_alphabet,
                                 typename boost::uint_t<p_column_byte_width * 8>::least>;
    /// Packed storage-unit type supplied by the file reader.
    using col_unit_t = typename col_cfg::storage_unit;
    static constexpr std::size_t col_lpu =
        compact_sequence_range<col_cfg>::traits::letters_per_unit;
    static constexpr std::uint8_t k_column_alphabet_code =
        pgseq_supported_alphabets::index_of<p_alphabet>();

    /// Load context that outlives the sink.
    load_context& ctx;
    /// Parent-row writer that outlives the sink.
    pg_identifier_writer& tuple_writer;
    /// Caller-owned completed-record counter.
    std::int64_t& records;
    /// Admitted or private-file raw header ownership remains alive until the next record.
    bioseq::io::admitted_byte_buffer header_owner;
    std::string_view header;
    /// Current logical sequence length in letters.
    sequence_size_t total_bp = 0;
    /// Writer that stores fragments as this record arrives.
    bioseq::storage::sequence_stream_writer writer;
    bioseq::storage::sequence_stream_writer quality_writer;
    std::unique_ptr<load_quality_encoder> quality_encoder = make_load_quality_encoder(ctx.cols);
    std::optional<idp::text_record_shape> preserved_shape;
    std::optional<idp::canonical_header> canonical_header;
    std::unique_ptr<bioseq::io::decoded_record> preserved_ranks;
    std::uint64_t observed_quality{};

    /// Discard local provisional data on every exit. PostgreSQL error rollback
    /// removes fragment rows; only end_record publishes a parent/source locator.
    ~load_stream_sink() {
        writer.abort();
        quality_writer.abort();
    }

    /// Copy the header and start storing a new sequence version.
    /// @param h Header bytes copied into this sink.
    /// @throws postgres_error If the header is too long or writer setup fails.
    void begin_record_owned(bioseq::io::admitted_byte_buffer&& incoming_header) {
        header_owner = std::move(incoming_header);
        const auto h = header_owner.text();
        header = h;
        total_bp = 0;
        observed_quality = 0;
        preserved_shape.reset();
        preserved_ranks.reset();
        canonical_header.reset();
        auto parsed_header = idp::parse_canonical_header(h);
        if (!parsed_header) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, parsed_header.error());
        canonical_header = std::move(*parsed_header);
        if (canonical_header) {
            constexpr std::size_t scalar_capacity = 4096u;
            auto scalar_permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(scalar_capacity * 4u);
            if (!scalar_permit) raise_load_error_as_cpp(ERRCODE_OUT_OF_MEMORY, "cannot admit canonical descriptor scratch");
            preserved_shape.emplace();
            const auto parsed = idp::decode_preservation_record(canonical_header->payload,
                *preserved_shape, true, scalar_capacity);
            if (!parsed) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, parsed.error());
            const auto& sequence = preserved_shape->sequences[0];
            const auto& quality = preserved_shape->sequences[1];
            if (sequence.alphabet_code && *sequence.alphabet_code != k_column_alphabet_code)
                raise_load_error_as_cpp(ERRCODE_DATATYPE_MISMATCH, "canonical sequence alphabet differs from destination");
            if (quality.alphabet_code && ctx.cols.quality_attno
                && *quality.alphabet_code != ctx.cols.quality_alphabet_code)
                raise_load_error_as_cpp(ERRCODE_DATATYPE_MISMATCH, "canonical quality alphabet differs from destination");
            if (quality.present && quality.storage == bioseq::io::preservation_storage::rank_hex) {
                preserved_ranks = std::make_unique<bioseq::io::decoded_record>();
                const auto decoded = idp::decode_preservation_record(canonical_header->payload,
                    *preserved_ranks, true, scalar_capacity);
                if (!decoded) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, decoded.error());
            }
            canonical_header->payload_storage = bioseq::io::admitted_byte_buffer{};
            canonical_header->payload = {};
        }
        if (!preserved_shape || preserved_shape->sequences[0].present) writer.begin(ctx.main_relid,
                     ctx.db_oid,
                     ctx.spc_oid,
                     ctx.main_relfilenode,
                     ctx.main_storage_id,
                     ctx.cols.seq_attno,
                     ctx.parent_td,
                     k_column_alphabet_code,
                     static_cast<std::uint8_t>(p_column_byte_width),
                     ctx.cap_bytes,
                     ctx.caches);
        if (quality_encoder && (!preserved_shape || (preserved_shape->sequences[1].present && !preserved_ranks))) {
            quality_encoder->reset();
            quality_writer.begin(ctx.main_relid, ctx.db_oid, ctx.spc_oid, ctx.main_relfilenode,
                ctx.main_storage_id, ctx.cols.quality_attno, ctx.parent_td,
                ctx.cols.quality_alphabet_code, ctx.cols.quality_byte_width, ctx.cap_bytes, ctx.caches);
        }
    }

    void quality_chunk(std::span<const std::uint8_t> bytes, std::size_t offset) {
        if (!quality_encoder) raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, "FASTQ requires a quality destination");
        if (offset != observed_quality)
            raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "quality chunks arrived out of order");
        if (preserved_shape && (!preserved_shape->sequences[1].present || preserved_ranks)) {
            const auto original = preserved_ranks
                ? preserved_ranks->sequence_source(bioseq::io::record_field_role::quality)
                : std::optional<bioseq::storage::borrowed_rank_window_source>{};
            validate_preserved_text_quality(original, bytes, offset);
        } else quality_encoder->append(bytes, offset, header, quality_writer);
        observed_quality += bytes.size();
    }

    /// Append packed sequence data to the fragment writer.
    /// @param c Borrowed packed units and their logical length in letters.
    /// @throws postgres_error If a fragment, write fails.
    void chunk(bioseq::io::packed_sequence_chunk<col_unit_t> c) {
        const std::size_t bytes =
            ((static_cast<std::size_t>(c.logical_length) + col_lpu - 1) / col_lpu) *
            p_column_byte_width;
        const auto* p = reinterpret_cast<const std::uint8_t*>(c.words.data());
        if (preserved_shape && !preserved_shape->sequences[0].present && c.logical_length != 0)
            raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "canonical NULL sequence must have an empty native body");
        if (!preserved_shape || preserved_shape->sequences[0].present)
            writer.append_packed_bytes(std::span<const std::uint8_t>{p, bytes}, c.logical_length);
        total_bp += c.logical_length;
    }

    /// Finish the fragments and write the parent row before marking its source ready.
    /// @throws postgres_error If finishing the sequence, writing the row or  publication fails.
    void end_record() {
        const std::uint16_t last_pad =
            (total_bp == 0) ? 0
                            : static_cast<std::uint16_t>(
                                  ((total_bp + col_lpu - 1) / col_lpu) * col_lpu - total_bp);
        if (preserved_shape) {
            const auto& shape = *preserved_shape;
            if (shape.sequences[0].present && shape.sequences[1].present
                && shape.sequences[0].original_length != shape.sequences[1].original_length)
                raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION,
                    "canonical sequence and quality original lengths disagree");
            const auto expected = shape.sequences[0].present ? shape.sequences[0].original_length : 0;
            if (total_bp != expected)
                raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "canonical sequence body differs from its original length");
            if (quality_encoder && observed_quality != total_bp)
                raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "canonical quality body length differs from sequence body");
            if (quality_encoder && shape.sequences[1].present && !preserved_ranks
                && shape.sequences[1].original_length != observed_quality)
                raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "canonical quality body differs from its original length");
        }
        std::optional<bioseq::storage::pending_sequence_source> pending_source;
        if (!preserved_shape || preserved_shape->sequences[0].present)
            pending_source.emplace(writer.finish(total_bp, last_pad));
        std::optional<bioseq::storage::pending_sequence_source> pending_quality;
        std::uint16_t quality_padding = 0;
        if (quality_encoder && (!preserved_shape || preserved_shape->sequences[1].present)) {
            inject_load_failure(1);
            if (preserved_ranks) {
                const auto original = preserved_ranks->sequence_source(bioseq::io::record_field_role::quality);
                if (!original) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "preserved quality ranks are incomplete");
                auto staged = stage_loaded_sequence(ctx, *original, ctx.cols.quality_attno,
                    ctx.cols.quality_alphabet_code, ctx.cols.quality_byte_width);
                quality_padding = staged.stub->padding_letters;
                pending_quality.emplace(std::move(staged.pending));
            } else {
                quality_padding = quality_encoder->finish(total_bp, quality_writer);
                pending_quality.emplace(quality_writer.finish(total_bp, quality_padding));
            }
        }
        write_parent_row(ctx,
                         tuple_writer,
                         header,
                         k_column_alphabet_code,
                         static_cast<std::uint8_t>(p_column_byte_width),
                         last_pad,
                         total_bp,
                         pending_source ? &*pending_source : nullptr, pending_quality ? &*pending_quality : nullptr, quality_padding,
                         preserved_shape && preserved_shape->header_is_null, preserved_shape.has_value(),
                         canonical_header ? &*canonical_header : nullptr);
        ++records;
    }
};

/** @brief A queued record event that owns its header or packed sequence data. */
/// @tparam p_storage_unit Packed unit passed from the parser to the writer.
template <typename p_storage_unit> struct queued_load_event {
    /// Action the writer performs for this event.
    enum class kind_t : std::uint8_t {
        /// Begin a record using `header`.
        begin_record,
        /// Append `words` representing `bp_count` logical letters.
        chunk,
        quality_chunk,
        /// Finish the active record.
        end_record,
    };

    /// Operation selected for this event.
    kind_t kind = kind_t::chunk;
    /// Owning record header for `begin_record`.
    bioseq::io::admitted_byte_buffer header{};
    /// Owning packed units for `chunk`.
    std::vector<p_storage_unit> words{};
    std::vector<std::uint8_t> quality{};
    std::size_t quality_offset{};
    /// Logical letters represented by `words`.
    std::uint64_t bp_count = 0;
    /// Whether the reader marked tqis as the final chunk of a record
    bool end_of_record = false;
};

/** @brief Pass parsed chunks to the PostgreSQL writer through a queue with limited capacity. */
/// @tparam p_storage_unit Packed unit owned by each queued event.
///
/// Push, pop and close may run on different threads. Events own their data
/// until pop transfers them to the writer.
template <typename p_storage_unit> class load_event_queue {
  public:
    /// Create a queue with a fixed event  limit.
    /// @param capacity Maximum queued events. Must be greater than zero.
    explicit load_event_queue(std::size_t capacity) : capacity_(capacity) {}

    /// Wait for queue space, then take ownership of an event.
    /// @param event Event moved into the queue on success.
    /// @return False if the queue closes before the event can be added.
    bool push(queued_load_event<p_storage_unit>&& event) {
        std::unique_lock<std::mutex> lock(mu_);
        const auto bytes = event.header.accounted_capacity_bytes() + event.words.size() * sizeof(p_storage_unit) + event.quality.size();
        not_full_.wait(lock, [this, bytes] { return closed_ || queue_.empty() ||
            (queue_.size() < capacity_ && queued_bytes_ <= byte_capacity_ && bytes <= byte_capacity_ - queued_bytes_); });
        if (closed_) {
            return false;
        }
        queued_bytes_ += bytes;
        queue_.push_back(std::move(event));
        not_empty_.notify_one();
        return true;
    }

    /// Wait for the next event or the end of the stream.
    /// @param event Receives ownership of the next queued event.
    /// @return False once the closed queue is empty.
    bool pop(queued_load_event<p_storage_unit>& event) {
        std::unique_lock<std::mutex> lock(mu_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        queued_bytes_ -= queue_.front().header.accounted_capacity_bytes() + queue_.front().words.size() * sizeof(p_storage_unit)
            + queue_.front().quality.size();
        event = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    /// Close the queue and wake every blocked producer or consumer.
    ///
    /// Already queued events remain available to `pop`.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

  private:
    const std::size_t capacity_;
    static constexpr std::size_t byte_capacity_ = 16u * 1024u * 1024u;
    std::size_t queued_bytes_{};
    std::mutex mu_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<queued_load_event<p_storage_unit>> queue_;
    bool closed_ = false;
};

/** @brief Stop the parser after the writer closes the queue on failure. */
struct load_queue_closed {};

/** @brief Close the queue when the parser task exits, including after an error. */
/// @tparam p_storage_unit Packed unit stored by the queue.
template <typename p_storage_unit> struct load_queue_close_scope {
    /// Queue that outlives this  cleanup object.
    load_event_queue<p_storage_unit>& queue;
    /// Close the queue on every producer exit path.
    ~load_queue_close_scope() noexcept {
        queue.close();
    }
};

/** @brief Turn parser callbacks into events that own their data ungil the writer reads them. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
template <sequence_alphabet p_alphabet, std::size_t p_column_byte_width>
struct queued_load_stream_sink {
    using col_cfg =
        sequence_custom_alphabet<p_alphabet,
                                 typename boost::uint_t<p_column_byte_width * 8>::least>;
    /// Packed storage-unit type carried by queue events.
    using col_unit_t = typename col_cfg::storage_unit;

    /// Thread-safe queue that outlives this producer sink.
    load_event_queue<col_unit_t>& queue;

    /// Wait to add an event, or stop if the writer has closed the queue.
    /// @param event Event transferred to the queue on success
    /// @throws load_queue_closed If the queue has closed.
    void push_or_closed(queued_load_event<col_unit_t>&& event) {
        if (!queue.push(std::move(event))) {
            throw load_queue_closed{};
        }
    }

    /// Enqueue a copied record header.
    /// @param h Header bytes copied before return.
    void begin_record_owned(bioseq::io::admitted_byte_buffer&& header) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::begin_record;
        event.header = std::move(header);
        push_or_closed(std::move(event));
    }

    /// Enqueue an owning copy of one borrowed reader chunk.
    /// @param c Packed units and logical letter count.
    void chunk(bioseq::io::packed_sequence_chunk<col_unit_t> c) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::chunk;
        event.words.assign(c.words.begin(), c.words.end());
        event.bp_count = c.logical_length;
        event.end_of_record = c.end_of_record;
        push_or_closed(std::move(event));
    }

    /// Transfer a packed chunk to the queue without copying its buffer.
    /// @param words Packed units moved into the event.
    /// @param bp_count Number of sequence letters in words.
    /// @param end_of_record Whether, this is the record's final chunk.
    void chunk_owned(std::vector<col_unit_t>&& words, std::uint64_t bp_count, bool end_of_record) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::chunk;
        event.words = std::move(words);
        event.bp_count = bp_count;
        event.end_of_record = end_of_record;
        push_or_closed(std::move(event));
    }

    void quality_chunk(std::span<const std::uint8_t> bytes, std::size_t offset) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::quality_chunk;
        event.quality.assign(bytes.begin(), bytes.end());
        event.quality_offset = offset;
        push_or_closed(std::move(event));
    }

    /// Tell the writer that the current  record is complete.
    void end_record() {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::end_record;
        push_or_closed(std::move(event));
    }
};

/** @brief Parse on a worker while the PostgreSQL thread writes queued sequence chunks. */
/// The queue limits how far parsing can run ahead of storage writes. After a
/// write failure, close it and wait for the parser before rethrowing the error.
/// PostgreSQL calls stay on the caller.
/// @tparam p_alphabet Sequence alphabet.
/// @tparam p_column_byte_width Packed unit width.
/// @tparam p_file_format FASTA or FASTQ.
/// @param ctx Caller-thread load context.
/// @param tuple_writer Caller-thread row writer.
/// @param file_path Path borrowed until parsing finishes.
/// @return Complete records written.
/// @throws postgres_error If parsing or writing fails.
template <sequence_alphabet p_alphabet,
          std::size_t p_column_byte_width,
          bioseq::io::sequence_file_format p_file_format>
[[nodiscard]] std::int64_t load_records_overlapped(load_context& ctx,
                                                   pg_identifier_writer& tuple_writer,
                                                   const char* file_path) {
    using col_cfg =
        sequence_custom_alphabet<p_alphabet,
                                 typename boost::uint_t<p_column_byte_width * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;

    load_event_queue<col_unit_t> queue{/*capacity=*/16};
    std::string io_error;
    auto io_error_kind = bioseq::io::input_failure_kind::other;
    std::string result_error;
    std::string producer_exception;
    std::optional<std::size_t> producer_records;

    // Construct every potentially throwing backend-owned sink before submitting
    // a producer that can block on queue capacity. In particular, quality codec
    // allocation must not escape while an unjoined worker borrows the queue.
    std::int64_t records = 0;
    load_stream_sink<p_alphabet, p_column_byte_width> sink{ctx, tuple_writer, records};
    queued_load_event<col_unit_t> event;

    auto& pool = bioseq::parallelism::global_pool::instance();
    auto group = pool.make_task_group();
    group.run(bioseq::parallelism::task_body{
        [&queue, &io_error, &io_error_kind, &result_error, &producer_exception, &producer_records, file_path] {
            load_queue_close_scope<col_unit_t> close_scope{queue};
            try {
                queued_load_stream_sink<p_alphabet, p_column_byte_width> sink{queue};
                // Preserve the first asynchronous reader diagnostic.
                auto on_fail = [&io_error, &io_error_kind](std::string&& msg,
                        bioseq::io::input_failure_kind kind = bioseq::io::input_failure_kind::other) {
                    if (io_error.empty()) {
                        io_error = std::move(msg);
                        io_error_kind = kind;
                    }
                };
                bioseq::io::reader_options opts = bioseq::io::default_reader_options;
                opts.concurrency.worker_count = 0;
                opts.concurrency.parser_count = 1;
                auto result =
                    bioseq::io::stream_sequences_file<p_alphabet, col_unit_t, p_file_format>(
                        file_path, sink, on_fail, opts);
                if (result) {
                    producer_records = *result;
                } else {
                    result_error = result.error();
                }
            } catch (const load_queue_closed&) {
                // Queue closure means the writer already has an error to report.
                // Let the caller  rethrow it after this parser task finishes.
            } catch (const std::exception& ex) {
                producer_exception = ex.what();
            } catch (...) {
                producer_exception = "unknown C++ exception";
            }
        }});

    std::exception_ptr consumer_error;
    try {
        while (queue.pop(event)) {
            switch (event.kind) {
            case queued_load_event<col_unit_t>::kind_t::begin_record:
                sink.begin_record_owned(std::move(event.header));
                break;
            case queued_load_event<col_unit_t>::kind_t::chunk:
                sink.chunk(bioseq::io::packed_sequence_chunk<col_unit_t>{
                    std::span<const col_unit_t>(event.words.data(), event.words.size()),
                    event.bp_count,
                    event.end_of_record});
                break;
            case queued_load_event<col_unit_t>::kind_t::quality_chunk:
                sink.quality_chunk(event.quality, event.quality_offset);
                break;
            case queued_load_event<col_unit_t>::kind_t::end_record:
                sink.end_record();
                break;
            }
        }
    } catch (...) {
        consumer_error = std::current_exception();
        queue.close();
    }
    group.wait();
    if (consumer_error != nullptr) {
        std::rethrow_exception(consumer_error);
    }

    if (!producer_exception.empty()) {
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION,
                                fmt::format("producer failed: {}", producer_exception));
    }
    if (!io_error.empty()) {
        raise_load_reader_failure(io_error_kind, io_error, ctx.cols);
    }
    if (!result_error.empty()) {
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, result_error);
    }
    if (producer_records.has_value() && *producer_records != static_cast<std::size_t>(records)) {
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION,
                                fmt::format("producer/consumer record count mismatch "
                                            "(producer={}, consumer={})",
                                            *producer_records,
                                            static_cast<long long>(records)));
    }

    bioseq_log<logging_level::debug>("[bioseq.load] overlapped parser/writer path records={}",
                                     static_cast<long long>(records));
    return records;
}

/** @brief Stream records, using a parser worker when overlap is enabled and available. */
/// @tparam p_alphabet Sequence alphabet.
/// @tparam p_column_byte_width Packed unit width in bytes.
/// @tparam p_file_format FASTA or FASTQ reader.
/// @param ctx Active load context.
/// @param tuple_writer Parent-row wrixer.
/// @param file_path Input path borrowed until the call returns.
/// @return Number of complete records written.
/// @throws postgres_error If parsing or writing fails.
template <sequence_alphabet p_alphabet,
          std::size_t p_column_byte_width,
          bioseq::io::sequence_file_format p_file_format>
[[nodiscard]] std::int64_t
load_records(load_context& ctx, pg_identifier_writer& tuple_writer, const char* file_path) {
    using col_cfg =
        sequence_custom_alphabet<p_alphabet,
                                 typename boost::uint_t<p_column_byte_width * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;

    constexpr std::size_t k_fragment_units = bioseq::storage::k_sequence_fragment_storage_units;
    static_assert(k_fragment_units * 8 <= 8192, "fragment payload must stay <= 8 KiB");

    if (bioseq::settings::load::write_overlap::enabled) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { bioseq::postgres::scheduler::backend_sync(); });
        auto& pool = bioseq::parallelism::global_pool::instance();
        const bool may_overlap = bioseq::parallelism::has_parallel_dispatch(pool.get_mode()) &&
                                 pool.maximum_concurrency() > 1;
        if (may_overlap) {
            return load_records_overlapped<p_alphabet, p_column_byte_width, p_file_format>(
                ctx, tuple_writer, file_path);
        }
    }

    std::int64_t records = 0;
    std::string io_error;
    auto io_error_kind = bioseq::io::input_failure_kind::other;
    load_stream_sink<p_alphabet, p_column_byte_width> sink{ctx, tuple_writer, records};
    // Preserve the first direct-reader diagnostic.
    auto on_fail = [&io_error, &io_error_kind](std::string&& msg,
            bioseq::io::input_failure_kind kind = bioseq::io::input_failure_kind::other) {
        if (io_error.empty()) {
            io_error = std::move(msg);
            io_error_kind = kind;
        }
    };

    bioseq::io::reader_options opts = bioseq::io::default_reader_options;
    opts.concurrency.worker_count = 0;
    opts.concurrency.parser_count = 1;
    auto result = bioseq::io::stream_sequences_file<p_alphabet, col_unit_t, p_file_format>(
        file_path, sink, on_fail, opts);

    if (!io_error.empty()) {
        raise_load_reader_failure(io_error_kind, io_error, ctx.cols);
    }
    if (!result) {
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, result.error());
    }
    return records;
}

// Choose the reader for the column alphabet, packed unit width and file format

namespace load_dispatch {
namespace tpl = bioseq::common::templating;

/** @brief Call the file reader for a fixed alphabet and packing width. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
template <class p_alphabet, std::size_t p_column_byte_width> struct file_format_adapter {
    /// Load records using the selected FASTA or FASTQ reader.
    /// @tparam p_file_format Input  file format.
    /// @return Number of records written.
    template <bioseq::io::sequence_file_format p_file_format>
    [[gnu::always_inline]] std::int64_t
    operator()(std::integral_constant<bioseq::io::sequence_file_format, p_file_format>,
               load_context& ctx,
               pg_identifier_writer& tuple_writer,
               const char* file_path) const {
        return load_records<p_alphabet, p_column_byte_width, p_file_format>(
            ctx, tuple_writer, file_path);
    }
};

/** @brief Choose FASTA or FASTQ reading for a fixed alphabet and packing width. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
template <class p_alphabet, std::size_t p_column_byte_width> struct format_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    /// Run the selected file reader and store its record count in out.
    void run() const {
        file_format_adapter<p_alphabet, p_column_byte_width> adapter{};
        out = tpl::dispatch_exact_value_or_default<std::int64_t,
                                                   bioseq::io::sequence_file_format,
                                                   bioseq::io::sequence_file_format::fasta>(
            tpl::value_sequence<bioseq::io::sequence_file_format,
                                bioseq::io::sequence_file_format::fasta,
                                bioseq::io::sequence_file_format::fastq>{},
            format,
            adapter,
            ctx,
            tuple_writer,
            file_path);
    }
};

/** @brief Continue reader selection with the column's packing width. */
/// @tparam p_alphabet Logical sequence alphabet.
template <class p_alphabet> struct width_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    /// Choose the file reader for this packed unit width.
    /// @tparam p_column_byte_width Packed unit width in bytes.
    template <std::size_t p_column_byte_width> void operator()() const {
        format_dispatch<p_alphabet, p_column_byte_width>{ctx, tuple_writer, file_path, format, out}
            .run();
    }
};

/** @brief Continue, reader selection with the column's sequence alphabet. */
struct alphabet_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    /// Select the packing width for this alphabet.
    /// @tparam p_alphabet Sequence alphabet.
    template <class p_alphabet> void operator()() const {
        dispatch_storage_unit_byte_width(
            ctx.cols.byte_width,
            width_dispatch<p_alphabet>{ctx, tuple_writer, file_path, format, out});
    }
};
} // namespace load_dispatch

/** @brief Select a reader for the column encoding and input format, then  load the file. */
/// @param ctx Load context with the target alphabet and packing width.
/// @param tuple_writer Parent-row writer.
/// @param file_path Input path borrowed until the call returns.
/// @param kind Detected file foymat and compression information.
/// @return Number of complete records written.
/// @throws postgres_error If the encoding is unsupported or parsing or writing fails.
[[nodiscard]] std::int64_t dispatch_and_stream(load_context& ctx,
                                               pg_identifier_writer& tuple_writer,
                                               const char* file_path,
                                               bioseq::io::sequence_file_kind kind) {
    if (static_cast<std::size_t>(ctx.cols.alphabet_code) >=
            pgseq_supported_biological_alphabets::alphabet_count) {
        raise_load_error_as_cpp(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            "FASTA/FASTQ loading requires a biological sequence alphabet; use SQL text input or COPY for CIGAR");
    }
    std::int64_t records_loaded = 0;
    pgseq_supported_biological_alphabets::dispatch_alphabet_by_code(
        ctx.cols.alphabet_code,
        load_dispatch::alphabet_dispatch{
            ctx, tuple_writer, file_path, kind.format, records_loaded});
    return records_loaded;
}
