/**
 * @file stream_dispatch.cppm
 * @brief Dispatches packed sequence streams by alphabet, storage width, and file format.
 */
module;
// Highway and PostgreSQL declarations precede the module unit because this
// owner imports Highway-bearing module graphs.
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
#include "catalog/namespace.h"   // (parity w/ sample_table_udf; relid arrives as regclass)
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
#include "utils/snapmgr.h" // PushActiveSnapshot/PopActiveSnapshot/GetTransactionSnapshot, ActiveSnapshotSet
#include "utils/typcache.h" // lookup_rowtype_tupdesc

#include "fmgr.h"
#include "rewrite/rewriteHandler.h" // build_column_default
}
// flush_relation_write_state declarations
export module bioseq.postgres.storage.io:stream_dispatch;

import bioseq.common.logging; // bioseq_log<logging_level::debug>(bioseq::settings::diagnostics::logging::enabled)
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
import bioseq.sequences;                       // compact_sequence_stdc_type_params::decode,
                                               //   dispatch_storage_unit_byte_width, traits
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, pgseq_supported_alphabets
import bioseq.postgres.storage.manager; // alloc_locator_stub_in, alloc_seq_caches, seq_caches,
                                        //   read_max_sub_table_cap_bytes,
//   k_sequence_fragment_storage_units, sequence_stream_writer,
//   flush_seq_cache_write_states, sequence_locator
import bioseq.postgres.table.io; // write_column_row, get_column_write_per_tuple_context
import bioseq.postgres.storage.indexing.admin; // build_policy_update_on_insert_indexes
import bioseq.io.options;                      // sequence_file_format, reader_options
import bioseq.io.file_kind;                    // detect_sequence_file_kind, sequence_file_kind
import bioseq.io.reader;                       // stream_sequences_file, packed_sequence_chunk,
                                               //   sample_sequence_headers
import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser; // resolve_identifier_mapping, parse_and_route_header
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
// initialize_column_write_state +
// pop_relation_column_write_state
// (PostgreSQL support already imported above: bioseq::postgres::support::find_primary_key_oid)
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;
import :typed_row_writer;

// --------------------------------------------------------------------------
// Bounded packed streaming and record loading
// --------------------------------------------------------------------------

/** @brief Streams packed records directly into sequence storage and parent rows. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
template <sequence_alphabet p_alphabet, std::size_t p_column_byte_width> struct load_stream_sink {
    /// Column-specific packed sequence configuration.
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
    /// Incremental fragment writer for the current record.
    bioseq::storage::sequence_stream_writer writer;

    /// Start one record and reserve its source epoch.
    /// @param h Header bytes copied into this sink.
    /// @throws PostgreSQL ERROR when the header or writer setup is invalid.
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

    /// Append one packed reader chunk.
    /// @param c Borrowed packed units with logical length measured in letters.
    /// @throws PostgreSQL ERROR when fragment persistence fails.
    void chunk(bioseq::io::packed_sequence_chunk<col_unit_t> c) {
        const std::size_t bytes =
            ((static_cast<std::size_t>(c.logical_length) + col_lpu - 1) / col_lpu) *
            p_column_byte_width;
        const auto* p = reinterpret_cast<const std::uint8_t*>(c.words.data());
        writer.append_packed_bytes(std::span<const std::uint8_t>{p, bytes}, c.logical_length);
        total_bp += c.logical_length;
    }

    /// Finish fragment persistence, write the parent row, and publish the source.
    /// @throws PostgreSQL ERROR when finalization or parent-row insertion fails.
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

/** @brief Owning producer-to-consumer message for one stream callback. */
/// @tparam p_storage_unit Packed unit copied across the worker boundary.
template <typename p_storage_unit> struct queued_load_event {
    /// Callback operation represented by this message.
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
    /// Reader callback's terminal-chunk marker.
    bool end_of_record = false;
};

/** @brief Bounded blocking queue used by the overlapped load path. */
/// @tparam p_storage_unit Packed unit owned by each queued event.
///
/// Push, pop, and close are safe to call from different threads. Queue storage
/// owns every event until it is moved to the consumer.
template <typename p_storage_unit> class load_event_queue {
  public:
    /// Construct a queue with a fixed positive event capacity.
    /// @param capacity Maximum number of queued events.
    explicit load_event_queue(std::size_t capacity) : capacity_(capacity) {}

    /// Block until an event can be enqueued or the queue is closed.
    /// @param event Event moved into queue storage on success.
    /// @return `false` when close prevents the enqueue.
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

    /// Block until an event is available or a closed queue is drained.
    /// @param event Receives the next owning event on success.
    /// @return `false` when no event remains after closure.
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

/** @brief Signals that the consumer closed the load queue after a failure. */
struct load_queue_closed {};

/** @brief Closes a load queue when the producer task leaves its scope. */
/// @tparam p_storage_unit Packed unit stored by the queue.
template <typename p_storage_unit> struct load_queue_close_scope {
    /// Queue that outlives this guard.
    load_event_queue<p_storage_unit>& queue;
    /// Close the queue on every producer exit path.
    ~load_queue_close_scope() noexcept {
        queue.close();
    }
};

/** @brief Copies parser callbacks into bounded queue events. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
template <sequence_alphabet p_alphabet, std::size_t p_column_byte_width>
struct queued_load_stream_sink {
    using col_cfg =
        sequence_custom_alphabet<p_alphabet,
                                 typename boost::uint_t<p_column_byte_width * 8>::least>;
    /// Packed storage-unit type copied into queue events.
    using col_unit_t = typename col_cfg::storage_unit;

    /// Thread-safe queue that outlives this producer sink.
    load_event_queue<col_unit_t>& queue;

    /// Enqueue one event or signal consumer-side closure.
    /// @param event Owning event transferred to the queue.
    /// @throws load_queue_closed When the consumer has closed the queue.
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

    /// Enqueue an already owned packed chunk.
    /// @param words Packed units transferred into the event.
    /// @param bp_count Logical letters represented by `words`.
    /// @param end_of_record Reader callback's terminal-chunk marker.
    void chunk_owned(std::vector<col_unit_t>&& words, std::uint64_t bp_count, bool end_of_record) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::chunk;
        event.words = std::move(words);
        event.bp_count = bp_count;
        event.end_of_record = end_of_record;
        push_or_closed(std::move(event));
    }

    /// Enqueue the record-completion callback.
    void end_record() {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::end_record;
        push_or_closed(std::move(event));
    }
};

/** @brief Runs parsing and PostgreSQL writes as a bounded producer/consumer pair. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
/// @tparam p_file_format Compile-time FASTA or FASTQ reader format.
/// @param ctx Active PostgreSQL load context used only on the caller thread.
/// @param tuple_writer Parent-row sink used only on the caller thread.
/// @param file_path Null-terminated path borrowed until the producer joins.
/// @return Number of parent records written.
/// @throws PostgreSQL ERROR for reader, producer, queue, or persistence failures.
///
/// The worker performs file parsing and owns queued copies. All PostgreSQL
/// operations remain on the calling backend thread.
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
                // The consumer raised a checked PostgreSQL/C++ exception and closed
                // the queue. Leave producer_exception empty so that exact exception
                // remains authoritative after the producer joins.
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

/** @brief Streams records through the configured direct or overlapped path. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
/// @tparam p_file_format Compile-time FASTA or FASTQ reader format.
/// @param ctx Active load context.
/// @param tuple_writer Parent-row sink.
/// @param file_path Null-terminated input path borrowed for the call.
/// @return Number of parent records written.
/// @throws PostgreSQL ERROR for reader or persistence failures.
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

// --------------------------------------------------------------------------
// Runtime-to-compile-time fan-out (alphabet × width × file type)
// --------------------------------------------------------------------------

namespace load_dispatch {
namespace tpl = bioseq::common::templating;

/** @brief Adapts one compile-time alphabet/width pair to format dispatch. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
template <class p_alphabet, std::size_t p_column_byte_width> struct file_format_adapter {
    /// Invoke the reader selected by a compile-time file-format tag.
    /// @tparam p_file_format FASTA or FASTQ reader format.
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

/** @brief Resolves a runtime file format for one alphabet/width pair. */
/// @tparam p_alphabet Logical sequence alphabet.
/// @tparam p_column_byte_width Packed storage-unit width in bytes.
template <class p_alphabet, std::size_t p_column_byte_width> struct format_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    /// Dispatch `format` and store the resulting record count in `out`.
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

/** @brief Resolves the encoded storage width for one alphabet. */
/// @tparam p_alphabet Logical sequence alphabet.
template <class p_alphabet> struct width_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    /// Continue dispatch for one supported byte width.
    /// @tparam p_column_byte_width Packed storage-unit width in bytes.
    template <std::size_t p_column_byte_width> void operator()() const {
        format_dispatch<p_alphabet, p_column_byte_width>{ctx, tuple_writer, file_path, format, out}
            .run();
    }
};

/** @brief Resolves the encoded sequence alphabet before width dispatch. */
struct alphabet_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    /// Continue dispatch for one registered alphabet.
    /// @tparam p_alphabet Logical sequence alphabet.
    template <class p_alphabet> void operator()() const {
        dispatch_storage_unit_byte_width(
            ctx.cols.byte_width,
            width_dispatch<p_alphabet>{ctx, tuple_writer, file_path, format, out});
    }
};
} // namespace load_dispatch

/** @brief Dispatches the resolved alphabet, storage width, and file format once. */
/// @param ctx Active load context containing resolved sequence encoding.
/// @param tuple_writer Parent-row sink.
/// @param file_path Null-terminated input path borrowed for the call.
/// @param kind Detected runtime file kind.
/// @return Number of parent records written.
/// @throws PostgreSQL ERROR for unsupported encoding, reader, or persistence failures.
[[nodiscard]] std::int64_t dispatch_and_stream(load_context& ctx,
                                               pg_identifier_writer& tuple_writer,
                                               const char* file_path,
                                               bioseq::io::sequence_file_kind kind) {
    std::int64_t records_loaded = 0;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        ctx.cols.alphabet_code,
        load_dispatch::alphabet_dispatch{
            ctx, tuple_writer, file_path, kind.format, records_loaded});
    return records_loaded;
}
