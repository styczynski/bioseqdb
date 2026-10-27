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
import bioseq.sequences;                       // Sequence alphabets and packed storage types.

import bioseq.postgres.types.compact_sequence; // PostgreSQL sequence values and supported alphabets.
import bioseq.postgres.storage.manager; // Sequence locators, fragment writers and relation caches.


import bioseq.postgres.table.io; // Column table row writes and per-row memory contexts.
import bioseq.io.options;                      // sequence_file_format, reader_options
import bioseq.io.file_kind;                    // detect_sequence_file_kind, sequence_file_kind
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

// Load records one packed chunk at a time.

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
    /// Owning copy of the current record header.
    std::string header;
    /// Current logical sequence length in letters.
    sequence_size_t total_bp = 0;
    /// Writer that stores fragments as this record arrives.
    bioseq::storage::sequence_stream_writer writer;

    /// Copy the header and start storing a new sequence version.
    /// @param h Header bytes copied into this sink.
    /// @throws postgres_error If the header is too long or writer setup fails.
    void begin_record(std::string_view h) {
        if (h.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                    "FASTA/FASTQ header exceeds INT_MAX");
        }
        header.assign(h);
        total_bp = 0;
        writer.begin(ctx.main_relid,
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
    }

    /// Append packed sequence data to the fragment writer.
    /// @param c Borrowed packed units and their logical length in letters.
    /// @throws postgres_error If a fragment, write fails.
    void chunk(bioseq::io::packed_sequence_chunk<col_unit_t> c) {
        const std::size_t bytes =
            ((static_cast<std::size_t>(c.logical_length) + col_lpu - 1) / col_lpu) *
            p_column_byte_width;
        const auto* p = reinterpret_cast<const std::uint8_t*>(c.words.data());
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
        auto pending_source = writer.finish(total_bp, last_pad);
        write_parent_row(ctx,
                         tuple_writer,
                         header,
                         k_column_alphabet_code,
                         static_cast<std::uint8_t>(p_column_byte_width),
                         last_pad,
                         total_bp,
                         std::move(pending_source));
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
        /// Finish the active record.
        end_record,
    };

    /// Operation selected for this event.
    kind_t kind = kind_t::chunk;
    /// Owning record header for `begin_record`.
    std::string header{};
    /// Owning packed units for `chunk`.
    std::vector<p_storage_unit> words{};
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
        not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_) {
            return false;
        }
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
    void begin_record(std::string_view h) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::begin_record;
        event.header.assign(h);
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
    std::string result_error;
    std::string producer_exception;
    std::optional<std::size_t> producer_records;

    auto& pool = bioseq::parallelism::global_pool::instance();
    auto group = pool.make_task_group();
    group.run(bioseq::parallelism::task_body{
        [&queue, &io_error, &result_error, &producer_exception, &producer_records, file_path] {
            load_queue_close_scope<col_unit_t> close_scope{queue};
            try {
                queued_load_stream_sink<p_alphabet, p_column_byte_width> sink{queue};
                // Preserve the first asynchronous reader diagnostic.
                auto on_fail = [&io_error](std::string&& msg) {
                    if (io_error.empty()) {
                        io_error = std::move(msg);
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

    std::int64_t records = 0;
    load_stream_sink<p_alphabet, p_column_byte_width> sink{ctx, tuple_writer, records};
    queued_load_event<col_unit_t> event;
    std::exception_ptr consumer_error;
    try {
        while (queue.pop(event)) {
            switch (event.kind) {
            case queued_load_event<col_unit_t>::kind_t::begin_record:
                sink.begin_record(event.header);
                break;
            case queued_load_event<col_unit_t>::kind_t::chunk:
                sink.chunk(bioseq::io::packed_sequence_chunk<col_unit_t>{
                    std::span<const col_unit_t>(event.words.data(), event.words.size()),
                    event.bp_count,
                    event.end_of_record});
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
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, io_error);
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
    load_stream_sink<p_alphabet, p_column_byte_width> sink{ctx, tuple_writer, records};
    // Preserve the first direct-reader diagnostic.
    auto on_fail = [&io_error](std::string&& msg) {
        if (io_error.empty()) {
            io_error = std::move(msg);
        }
    };

    bioseq::io::reader_options opts = bioseq::io::default_reader_options;
    opts.concurrency.worker_count = 0;
    opts.concurrency.parser_count = 1;
    auto result = bioseq::io::stream_sequences_file<p_alphabet, col_unit_t, p_file_format>(
        file_path, sink, on_fail, opts);

    if (!io_error.empty()) {
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, io_error);
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
