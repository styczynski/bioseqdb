/**
 * @file index.cppm
 * @brief Stream FASTA and FASTQ records into PostgreSQL without whole-sequence materialization.
 */
module;
// Highway and PostgreSQL declarations precede the module unit because this
// owner imports Highway-bearing module graphs.
#include <libassert/assert.hpp>
#include <hwy/highway.h>
#include <bioseq/pg_compat.h>
#include <boost/integer.hpp>
#include <boost/container/small_vector.hpp>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
extern "C" {
#include "postgres.h"
#include "access/xact.h"       // CommandCounterIncrement
#include "fmgr.h"
#include "access/table.h"      // table_open / table_close
#include "access/relation.h"   // index_open / index_close
#include "catalog/namespace.h" // (parity w/ sample_table_udf; relid arrives as regclass)
#include "catalog/pg_type_d.h" // TEXTOID, VARCHAROID, BPCHAROID
#include "commands/sequence.h" // ResetSequenceCaches
#include "executor/tuptable.h" // TupleTableSlot, MakeTupleTableSlot, TTSOpsVirtual, Exec*Tuple
#include "executor/executor.h" // ExecConstraints, ExecInitExpr, ExecEvalExpr
#include "nodes/makefuncs.h"   // makeNode (ResultRelInfo)
#include "rewrite/rewriteHandler.h" // build_column_default
#include "optimizer/optimizer.h"    // expression_planner
#include "utils/builtins.h"    // text_to_cstring, cstring_to_text_with_len, regtypein
#include "utils/lsyscache.h"   // getTypeInputInfo, get_typtype, getBaseType
#include "utils/typcache.h"    // lookup_rowtype_tupdesc
#include "utils/array.h"       // deconstruct_array, ARR_ELEMTYPE
#include "utils/rel.h"         // RelationGetDescr
#include "utils/resowner.h"    // CurrentResourceOwner, TopTransactionResourceOwner
#include "utils/snapmgr.h"     // PushActiveSnapshot/PopActiveSnapshot/GetTransactionSnapshot, ActiveSnapshotSet
}
                               // flush_relation_write_state declarations
export module bioseq.postgres.storage.io;

import bioseq.common.logging;                           // bioseq_log<logging_level::debug>(bioseq::settings::diagnostics::logging::enabled)
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.configuration.core;
import bioseq.storage.size;                // sequence_size_t
import bioseq.storage.layout;
import bioseq.common.templating.runtime_dispatch; // dispatch_exact_value_or_default
import bioseq.common.templating.value_sequence;
import bioseq.sequences;                   // compact_sequence_stdc_type_params::decode,
                                           //   dispatch_storage_unit_byte_width, traits
import bioseq.postgres.types.compact_sequence;   // bioseq::pg_types::seq, pgseq_supported_alphabets
import bioseq.postgres.storage.manager;                     // alloc_locator_stub_in, alloc_seq_caches, seq_caches,
                                           //   read_max_sub_table_cap_bytes,
                                           //   k_sequence_fragment_storage_units, sequence_stream_writer,
                                           //   flush_seq_cache_write_states, sequence_locator
import bioseq.postgres.table.io;                    // write_column_row, get_column_write_per_tuple_context
import bioseq.postgres.storage.indexing.admin;      // build_policy_update_on_insert_indexes
import bioseq.io.options;                  // sequence_file_format, reader_options
import bioseq.io.file_kind;               // detect_sequence_file_kind, sequence_file_kind
import bioseq.io.reader;               // stream_sequences_file, packed_sequence_chunk,
                                           //   sample_sequence_headers
import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser;        // resolve_identifier_mapping, parse_and_route_header
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
                                           // initialize_column_write_state +
                                           // pop_relation_column_write_state
                                           // (PostgreSQL support already imported above: bioseq::postgres::support::find_primary_key_oid)
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.scheduler.supervisor;

namespace {

using bioseq::storage::sequence_size_t;   // bp counts (uint64 — 6 Gbp safe)
using column_mapping_row =
    bioseq::storage::load_pg::composite_rows::column_mapping;
using column_mapping_codec =
    bioseq::postgres::storage::io::sql_row_codecs::column_mapping_codec;

/// Raise one loader error through the checked PG leaf seam. The owned message
/// remains alive while PostgreSQL constructs/copies ErrorData, then ordinary
/// C++ unwinding releases every caller-owned queue/string/writer object.
[[noreturn]] void raise_load_error_as_cpp(int sqlstate, std::string message) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, &message] {
        ereport(ERROR,
            (errcode(sqlstate), errmsg("bioseq.load: %s", message.c_str())));
    });
    pg_unreachable();
}

// --------------------------------------------------------------------------
// Column discovery
// --------------------------------------------------------------------------

[[nodiscard]] Oid load_seq_type_oid() {
    return bioseq::postgres::support::invoke_pg_as_cpp([] {
        return bioseq::postgres::support::pg_datum::to_oid(
            bioseq::postgres::support::pg_datum::from_raw(
                DirectFunctionCall1(
                    regtypein,
                    bioseq::postgres::support::pg_datum::from_cstring(
                        "bioseq.seq").raw())));
    });
}

/** @brief Resolved destination sequence column and its encoded typmod facts. */
struct load_columns {
    std::int16_t header_attno;   // 0 = none (header discarded)
    std::int16_t seq_attno;      // the one bioseq.seq column (always required)
    std::uint8_t alphabet_code;  // from the seq column's typmod
    std::uint8_t byte_width;     // 1/2/4/8, from the seq column's typmod
};

/**
 * @brief Finds the single `bioseq.seq` column and decodes its typmod.
 * @throws postgres_error If the relation has no usable unique sequence column.
 */
[[nodiscard]] load_columns resolve_load_columns(Relation rel) {
    const TupleDesc td = RelationGetDescr(rel);
    const Oid seq_type_oid = load_seq_type_oid();
    if (!OidIsValid(seq_type_oid)) {
        raise_load_error_as_cpp(
            ERRCODE_UNDEFINED_OBJECT,
            "bioseq.seq type not found (is the extension installed?)");
    }

    std::int16_t header_attno = 0;
    std::int16_t seq_attno = 0;
    int seq_count = 0;
    for (int i = 0; i < td->natts; ++i) {
        const Form_pg_attribute attr = TupleDescAttr(td, i);
        if (attr->attisdropped || attr->attnum <= 0) {
            continue;
        }
        if (attr->atttypid == seq_type_oid) {
            ++seq_count;
            seq_attno = static_cast<std::int16_t>(i + 1);
        } else if ((attr->atttypid == TEXTOID || attr->atttypid == VARCHAROID
                   || attr->atttypid == BPCHAROID) && header_attno == 0) {
            header_attno = static_cast<std::int16_t>(i + 1);  // first text column
        }
    }
    // Exactly one seq column (unchanged).
    if (seq_count == 0) {
        raise_load_error_as_cpp(
            ERRCODE_INVALID_TABLE_DEFINITION,
            "table has no bioseq.seq column");
    }
    if (seq_count > 1) {
        raise_load_error_as_cpp(
            ERRCODE_INVALID_TABLE_DEFINITION,
            fmt::format(
                "table has {} bioseq.seq columns; exactly one is required",
                seq_count));
    }

    // (alphabet, width) off the seq column's typmod — NEVER hardcoded.
    const Form_pg_attribute seq_attr = TupleDescAttr(td, seq_attno - 1);
    if (seq_attr->atttypmod < 0) {
        raise_load_error_as_cpp(
            ERRCODE_INVALID_TABLE_DEFINITION,
            "bioseq.seq column has no typmod (declare it as "
            "bioseq.seq('<alphabet>', <width>))");
    }
    const auto tp = compact_sequence_stdc_type_params::decode(seq_attr->atttypmod);
    return load_columns{header_attno, seq_attno, tp.alphabet_code,
                       tp.storage_unit_byte_width};
}

// --------------------------------------------------------------------------
// Typed-column coercion and PostgreSQL row writing
// --------------------------------------------------------------------------
namespace idp = bioseq::io::identifier_parser;

// text_with_typmod = varchar(n)/bpchar(n) with a length constraint -> must run the
// input function so the length is enforced (NOT the raw cstring_to_text path).
/** @brief PostgreSQL coercion route selected for one destination column. */
enum class dest_kind : std::uint8_t {
    text,
    text_with_typmod,
    scalar,
    range,
    multirange,
};

/** @brief Cached input-function metadata for one mapped destination column. */
struct dest_coercion {
    std::int16_t attno;        // 1-based
    std::string  column_name;  // copied once for the ErrorContextCallback
    dest_kind    kind;
    Oid          input_fn;     // from getTypeInputInfo
    Oid          io_param;     // typioparam (enum type OID for enums)
    std::int32_t typmod;       // column atttypmod (varchar(n), numeric(p,s), …)
};

/** @brief Compiled default expression for one destination attribute. */
struct default_col {
    std::int16_t attno;
    ExprState* state;
};

/// Two-byte substring scan without relying on the POSIX `memmem` extension.
[[nodiscard]] const char* find_substr(const char* data, int len,
                                      const char* needle) {
    const int nlen = static_cast<int>(std::strlen(needle));
    if (nlen == 0 || len < nlen) {
        return nullptr;
    }
    for (int i = 0; i + nlen <= len; ++i) {
        if (std::memcmp(data + i, needle, static_cast<std::size_t>(nlen)) == 0) {
            return data + i;
        }
    }
    return nullptr;
}

/// Copy `[data,len)` into a per-tuple-palloc'd NUL-terminated cstring. For a
/// range/multirange column with exactly one `..`, rewrite `lo..hi` to the
/// canonical inclusive `[lo,hi]` (multirange: `{[lo,hi]}`); otherwise the trimmed
/// text is passed straight to `<type>range_in`/`<type>multirange_in`.
[[nodiscard]] char* make_cstr(const char* data, int len,
                              const dest_coercion& dc, MemoryContext mctx) {
    while (len > 0 && data[0] == ' ') {
        ++data;
        --len;
    }
    while (len > 0 && data[len - 1] == ' ') {
        --len;
    }
    const bool is_range = (dc.kind == dest_kind::range
                           || dc.kind == dest_kind::multirange);
    if (is_range) {
        const char* d0 = find_substr(data, len, "..");
        const bool one_dots = d0 &&
            !find_substr(d0 + 2, len - static_cast<int>(d0 - data) - 2, "..");
        if (d0 && one_dots) {
            const int la = static_cast<int>(d0 - data);
            const int hb_off = la + 2;
            const int hb = len - hb_off;
            const char* lo = data;
            int lol = la;
            while (lol != 0 && lo[0] == ' ') {
                ++lo;
                --lol;
            }
            while (lol != 0 && lo[lol - 1] == ' ') {
                --lol;
            }
            const char* hi = data + hb_off;
            int hil = hb;
            while (hil != 0 && hi[0] == ' ') {
                ++hi;
                --hil;
            }
            while (hil != 0 && hi[hil - 1] == ' ') {
                --hil;
            }
            const bool mr = (dc.kind == dest_kind::multirange);
            char* out = bioseq::postgres::support::invoke_pg_as_cpp(
                [mctx, lol, hil] {
                    return static_cast<char*>(MemoryContextAlloc(
                        mctx, static_cast<std::size_t>(lol + hil + 6)));
                });
            int k = 0;
            if (mr) {
                out[k++] = '{';
            }
            out[k++] = '[';
            std::memcpy(out + k, lo, static_cast<std::size_t>(lol)); k += lol;
            out[k++] = ',';
            std::memcpy(out + k, hi, static_cast<std::size_t>(hil)); k += hil;
            out[k++] = ']';
            if (mr) {
                out[k++] = '}';
            }
            out[k] = '\0';
            return out;
        }
        // 0 or >1 ".." -> hand the trimmed text to <type>range_in/<type>multirange_in.
    }
    char* out = bioseq::postgres::support::invoke_pg_as_cpp([mctx, len] {
        return static_cast<char*>(MemoryContextAlloc(
            mctx, static_cast<std::size_t>(len) + 1));
    });
    std::memcpy(out, data, static_cast<std::size_t>(len));
    out[len] = '\0';
    return out;
}

// --------------------------------------------------------------------------
// Load context
// --------------------------------------------------------------------------

/** @brief Invocation-owned PostgreSQL and parser state for one streamed load. */
struct load_context {
    Oid main_relid;                                  // parent table OID (for update_index)
    Oid db_oid, spc_oid; RelFileNumber main_relfilenode;
    std::uint64_t main_storage_id = 0;
    Relation parent_rel; TupleDesc parent_td;
    load_columns cols;
    bioseq::storage::seq_caches* caches;
    column_write_state* write_state;
    sequence_size_t cap_bytes;
    TupleTableSlot* slot;

    // Keep the PostgreSQL writer non-templated so identifier parsing is not
    // multiplied by the alphabet, storage-width, and file-type dispatch matrix.
    bool                       parser_active = false;
    idp::header_format         requested_fmt = idp::header_format::automatic;
    idp::header_format         effective_fmt = idp::header_format::automatic;
    idp::identifier_routing    routing;
    idp::identifier_parse_state parse_state;
    idp::identifier_writer_reference writer_ref;
    std::vector<dest_coercion> dest;                   // index == slot
    std::vector<default_col>   defaults;
    EState*                    estate = nullptr;
    ExprContext*               econtext = nullptr;
    ResultRelInfo*             result_rel_info = nullptr;

    // Read only by the per-record ErrorContextCallback.
    std::uint64_t              record_no = 0;
    std::string_view           current_header;
    const char*                current_column_name = nullptr;
};

/// Add bounded record context to coercion, default, or constraint failures.
void bioseq_load_error_context(void* arg) {
    auto* ctx = static_cast<load_context*>(arg);
    const std::string_view h = ctx->current_header;
    const int hlen = static_cast<int>(std::min<std::size_t>(h.size(), 240));
    if (ctx->current_column_name != nullptr) {
        errcontext("bioseq.load: record %llu, column \"%s\", header \"%.*s\"",
                   static_cast<unsigned long long>(ctx->record_no),
                   ctx->current_column_name, hlen, h.data());
    } else {
        errcontext("bioseq.load: record %llu, header \"%.*s\"",
                   static_cast<unsigned long long>(ctx->record_no), hlen, h.data());
    }
}

/** @brief Installs bounded record context for the duration of one row write. */
struct load_error_context_scope {
    load_context& ctx;
    ErrorContextCallback callback{};

    load_error_context_scope(load_context& context, std::string_view header)
        : ctx(context) {
        ctx.current_header = header;
        ctx.current_column_name = nullptr;
        callback.callback = bioseq_load_error_context;
        callback.arg = &ctx;
        callback.previous = error_context_stack;
        error_context_stack = &callback;
    }

    ~load_error_context_scope() noexcept {
        error_context_stack = callback.previous;
        ctx.current_header = {};
        ctx.current_column_name = nullptr;
    }
};

/// PostgreSQL sink that routes each byte range through its column's cached
/// input function.
struct pg_identifier_writer {
    load_context&                  ctx;
    std::span<const dest_coercion> dest;       // index == slot
    std::span<const default_col>   defaults;   // unmapped/unset columns w/ default

	void begin_tuple() {
	    bioseq::postgres::support::invoke_pg_as_cpp([this] {
	        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.clear_all_to_null();
	    });
	}

	void set_null(idp::destination_slot slot) {
	    bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_null(dest[slot].attno);
	}
    void set_field(idp::destination_slot slot, const char* data, std::int32_t len) {
        const dest_coercion& dc = dest[slot];
        ctx.current_column_name = dc.column_name.c_str();   // errcontext if input fn errors
        const MemoryContext per_tuple =
            get_column_write_per_tuple_context(ctx.write_state);
        Datum v;
        if (dc.kind == dest_kind::text) { // plain text/varchar/bpchar, no typmod
            v = bioseq::postgres::support::invoke_pg_as_cpp(
                [per_tuple, data, len] {
                    const MemoryContext old =
                        MemoryContextSwitchTo(per_tuple);
                    const Datum result = PointerGetDatum(
                        cstring_to_text_with_len(data, len));
                    MemoryContextSwitchTo(old);
                    return result;
                });
        } else { // text_with_typmod/scalar/range/multirange
            char* cstr = make_cstr(data, len, dc, per_tuple);
            v = bioseq::postgres::support::invoke_pg_as_cpp(
                [per_tuple, &dc, cstr] {
                    const MemoryContext old =
                        MemoryContextSwitchTo(per_tuple);
                    const Datum result = OidInputFunctionCall(
                        dc.input_fn, cstr, dc.io_param, dc.typmod);
                    MemoryContextSwitchTo(old);
                    return result;
                });
        }
	    bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
	        dc.attno, bioseq::postgres::support::pg_datum::from_raw(v));
	    ctx.current_column_name = nullptr;
	}
    void end_tuple() {
        // Defaults fill ONLY columns the parser left NULL — a mapped value always
        // wins over a column default. By-reference default datums live in the
        // econtext memory and stay valid until write_column_row copies them.
        for (const default_col& d : defaults) {
            if (!ctx.slot->tts_isnull[d.attno - 1]) {
                continue;
            }
	        bool isnull = false;
	        Datum v = bioseq::postgres::support::invoke_pg_as_cpp(
	            [&d, this, &isnull] {
	                return ExecEvalExpr(d.state, ctx.econtext, &isnull);
	            });
	        if (isnull) {
	            bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_null(d.attno);
	        } else {
	            bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
	                d.attno, bioseq::postgres::support::pg_datum::from_raw(v));
	        }
	    }
	    bioseq::postgres::support::invoke_pg_as_cpp([this] {
	        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.store_virtual();
	    });
        if (ctx.result_rel_info && ctx.parent_rel->rd_att->constr) {
            bioseq::postgres::support::invoke_pg_as_cpp([this] {
                ExecConstraints(ctx.result_rel_info, ctx.slot, ctx.estate);
            });
        }
        (void) write_column_row(
            ctx.write_state, ctx.slot->tts_values, ctx.slot->tts_isnull);
        // Reset temporary contexts only after write_column_row has copied
        // the by-reference datums.
        if (ctx.econtext) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { ResetExprContext(ctx.econtext); });
        }
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            MemoryContextReset(get_column_write_per_tuple_context(ctx.write_state));
        });
    }
    void finish() {}

    /** @return The number of identifier-parser destinations. */
    [[nodiscard]] std::size_t destination_count() const noexcept {
        return dest.size();
    }
};
static_assert(idp::identifier_destination_writer<pg_identifier_writer>);

// The non-templated parent writer keeps identifier parsing outside the
// alphabet, storage-width, and file-type dispatch matrix.
void write_parent_row(load_context& ctx, pg_identifier_writer& tuple_writer,
                      std::string_view header,
                      std::uint8_t alphabet_code, std::uint8_t byte_width,
                      std::uint16_t last_pad, sequence_size_t total_bp,
                      bioseq::storage::pending_sequence_source&& pending_source) {
    const bioseq::storage::sequence_locator locator = pending_source.locator();
    bioseq::pg_types::seq* stub = bioseq::postgres::support::invoke_pg_as_cpp(
        [write_state = ctx.write_state, alphabet_code, byte_width,
         last_pad, total_bp, locator] {
            return bioseq::storage::alloc_locator_stub_in(
                get_column_write_per_tuple_context(write_state),
                alphabet_code, byte_width, last_pad, total_bp, locator);
        });

    ++ctx.record_no;
    load_error_context_scope error_context{ctx, header};

	tuple_writer.begin_tuple();                             // ExecClearTuple + all-NULL
	bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
	    ctx.cols.seq_attno, bioseq::postgres::support::pg_datum::from_pointer(stub));

    if (ctx.parser_active) {
        auto routed = idp::parse_and_route_header(
            ctx.effective_fmt, header, ctx.routing, ctx.parse_state, ctx.writer_ref);
        if (!routed) {
            raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, routed.error());
        }
    } else if (ctx.cols.header_attno != 0) {
        if (header.size() > static_cast<std::size_t>(INT_MAX)) {
            raise_load_error_as_cpp(
                ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                "FASTA/FASTQ header exceeds INT_MAX");
        }
        text* htext = bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx, header] {
            const MemoryContext old = MemoryContextSwitchTo(
                get_column_write_per_tuple_context(ctx.write_state));
            text* result = cstring_to_text_with_len(
                header.data(), static_cast<int>(header.size()));
            MemoryContextSwitchTo(old);
            return result;
        });
	    bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
	        ctx.cols.header_attno, bioseq::postgres::support::pg_datum::from_pointer(htext));
    }
    tuple_writer.end_tuple();  // defaults, ExecConstraints, write_column_row, ctx reset
    pending_source.publish_after_parent_row();
}

// --------------------------------------------------------------------------
// Bounded packed streaming and record loading
// --------------------------------------------------------------------------

/** @brief Streams packed records directly into sequence storage and parent rows. */
template <sequence_alphabet p_alphabet, std::size_t p_column_byte_width>
struct load_stream_sink {
    using col_cfg = sequence_custom_alphabet<
        p_alphabet,
        typename boost::uint_t<p_column_byte_width * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;
    static constexpr std::size_t col_lpu =
        compact_sequence_range<col_cfg>::traits::letters_per_unit;
    static constexpr std::uint8_t k_column_alphabet_code =
        pgseq_supported_alphabets::index_of<p_alphabet>();

    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    std::int64_t& records;
    std::string header;
    sequence_size_t total_bp = 0;
    bioseq::storage::sequence_stream_writer writer;

    void begin_record(std::string_view h) {
        if (h.size() > static_cast<std::size_t>(INT_MAX)) {
            raise_load_error_as_cpp(
                ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                "FASTA/FASTQ header exceeds INT_MAX");
        }
        header.assign(h);
        total_bp = 0;
        writer.begin(ctx.main_relid, ctx.db_oid, ctx.spc_oid,
                     ctx.main_relfilenode, ctx.main_storage_id,
                     ctx.cols.seq_attno, ctx.parent_td, k_column_alphabet_code,
                     static_cast<std::uint8_t>(p_column_byte_width), ctx.cap_bytes,
                     ctx.caches);
    }

    void chunk(bioseq::io::packed_sequence_chunk<col_unit_t> c) {
        const std::size_t bytes =
            ((static_cast<std::size_t>(c.logical_length) + col_lpu - 1) / col_lpu)
            * p_column_byte_width;
        const auto* p = reinterpret_cast<const std::uint8_t*>(c.words.data());
        writer.append_packed_bytes(
            std::span<const std::uint8_t>{p, bytes}, c.logical_length);
        total_bp += c.logical_length;
    }

    void end_record() {
        const std::uint16_t last_pad = (total_bp == 0) ? 0
            : static_cast<std::uint16_t>(
                  ((total_bp + col_lpu - 1) / col_lpu) * col_lpu - total_bp);
        auto pending_source = writer.finish(total_bp, last_pad);
        write_parent_row(ctx, tuple_writer, header, k_column_alphabet_code,
                         static_cast<std::uint8_t>(p_column_byte_width), last_pad,
                         total_bp, std::move(pending_source));
        ++records;
    }
};

/** @brief Owning producer-to-consumer message for one stream callback. */
template <typename p_storage_unit>
struct queued_load_event {
    enum class kind_t : std::uint8_t {
        begin_record,
        chunk,
        end_record,
    };

    kind_t kind = kind_t::chunk;
    std::string header{};
    std::vector<p_storage_unit> words{};
    std::uint64_t bp_count = 0;
    bool end_of_record = false;
};

/** @brief Bounded blocking queue used by the overlapped load path. */
template <typename p_storage_unit>
class load_event_queue {
public:
    explicit load_event_queue(std::size_t capacity) : capacity_(capacity) {}

    bool push(queued_load_event<p_storage_unit>&& event) {
        std::unique_lock<std::mutex> lock(mu_);
        not_full_.wait(lock, [this] {
            return closed_ || queue_.size() < capacity_;
        });
        if (closed_) {
            return false;
        }
        queue_.push_back(std::move(event));
        not_empty_.notify_one();
        return true;
    }

    bool pop(queued_load_event<p_storage_unit>& event) {
        std::unique_lock<std::mutex> lock(mu_);
        not_empty_.wait(lock, [this] {
            return closed_ || !queue_.empty();
        });
        if (queue_.empty()) {
            return false;
        }
        event = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

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
template <typename p_storage_unit>
struct load_queue_close_scope {
    load_event_queue<p_storage_unit>& queue;
    ~load_queue_close_scope() noexcept { queue.close(); }
};

/** @brief Copies parser callbacks into bounded queue events. */
template <sequence_alphabet p_alphabet, std::size_t p_column_byte_width>
struct queued_load_stream_sink {
    using col_cfg = sequence_custom_alphabet<
        p_alphabet,
        typename boost::uint_t<p_column_byte_width * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;

    load_event_queue<col_unit_t>& queue;

    void push_or_closed(queued_load_event<col_unit_t>&& event) {
        if (!queue.push(std::move(event))) {
            throw load_queue_closed{};
        }
    }

    void begin_record(std::string_view h) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::begin_record;
        event.header.assign(h);
        push_or_closed(std::move(event));
    }

    void chunk(bioseq::io::packed_sequence_chunk<col_unit_t> c) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::chunk;
        event.words.assign(c.words.begin(), c.words.end());
        event.bp_count = c.logical_length;
        event.end_of_record = c.end_of_record;
        push_or_closed(std::move(event));
    }

    void chunk_owned(
            std::vector<col_unit_t>&& words,
            std::uint64_t bp_count,
            bool end_of_record) {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::chunk;
        event.words = std::move(words);
        event.bp_count = bp_count;
        event.end_of_record = end_of_record;
        push_or_closed(std::move(event));
    }

    void end_record() {
        queued_load_event<col_unit_t> event;
        event.kind = queued_load_event<col_unit_t>::kind_t::end_record;
        push_or_closed(std::move(event));
    }
};

/** @brief Runs parsing and PostgreSQL writes as a bounded producer/consumer pair. */
template <sequence_alphabet p_alphabet,
          std::size_t p_column_byte_width,
          bioseq::io::sequence_file_format p_file_format>
[[nodiscard]] std::int64_t load_records_overlapped(
        load_context& ctx,
        pg_identifier_writer& tuple_writer,
        const char* file_path) {
    using col_cfg = sequence_custom_alphabet<
        p_alphabet,
        typename boost::uint_t<p_column_byte_width * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;

    load_event_queue<col_unit_t> queue{/*capacity=*/16};
    std::string io_error;
    std::string result_error;
    std::string producer_exception;
    std::optional<std::size_t> producer_records;

    auto& pool = bioseq::parallelism::global_pool::instance();
    auto group = pool.make_task_group();
    group.run(bioseq::parallelism::task_body{[
            &queue, &io_error, &result_error, &producer_exception,
            &producer_records, file_path] {
        load_queue_close_scope<col_unit_t> close_scope{queue};
        try {
            queued_load_stream_sink<
                p_alphabet, p_column_byte_width> sink{queue};
            auto on_fail = [&io_error](std::string&& msg) {
                if (io_error.empty()) {
                    io_error = std::move(msg);
                }
            };
            bioseq::io::reader_options opts = bioseq::io::default_reader_options;
            opts.concurrency.worker_count = 0;
            opts.concurrency.parser_count = 1;
            auto result = bioseq::io::stream_sequences_file<
                p_alphabet, col_unit_t, p_file_format>(
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
    load_stream_sink<p_alphabet, p_column_byte_width> sink{
        ctx, tuple_writer, records};
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
        raise_load_error_as_cpp(
            ERRCODE_DATA_EXCEPTION,
            fmt::format("producer failed: {}", producer_exception));
    }
    if (!io_error.empty()) {
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, io_error);
    }
    if (!result_error.empty()) {
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, result_error);
    }
    if (producer_records.has_value()
        && *producer_records != static_cast<std::size_t>(records)) {
        raise_load_error_as_cpp(
            ERRCODE_DATA_EXCEPTION,
            fmt::format(
                "producer/consumer record count mismatch "
                "(producer={}, consumer={})",
                *producer_records, static_cast<long long>(records)));
    }

    bioseq_log<logging_level::debug>("[bioseq.load] overlapped parser/writer path records={}",
            static_cast<long long>(records));
    return records;
}

/** @brief Streams records through the configured direct or overlapped path. */
template <sequence_alphabet p_alphabet,
          std::size_t p_column_byte_width,
          bioseq::io::sequence_file_format p_file_format>
[[nodiscard]] std::int64_t load_records(
        load_context& ctx,
        pg_identifier_writer& tuple_writer,
        const char* file_path) {
    using col_cfg = sequence_custom_alphabet<
        p_alphabet,
        typename boost::uint_t<p_column_byte_width * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;

    constexpr std::size_t k_fragment_units =
        bioseq::storage::k_sequence_fragment_storage_units;
    static_assert(k_fragment_units * 8 <= 8192,
                  "fragment payload must stay <= 8 KiB");

    if (bioseq::settings::load::write_overlap::enabled) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { bioseq::postgres::scheduler::backend_sync(); });
        auto& pool = bioseq::parallelism::global_pool::instance();
        const bool may_overlap =
            bioseq::parallelism::has_parallel_dispatch(pool.get_mode())
            && pool.maximum_concurrency() > 1;
        if (may_overlap) {
            return load_records_overlapped<
                p_alphabet, p_column_byte_width, p_file_format>(
                ctx, tuple_writer, file_path);
        }
    }

    std::int64_t records = 0;
    std::string io_error;
    load_stream_sink<p_alphabet, p_column_byte_width> sink{
        ctx, tuple_writer, records};
    auto on_fail = [&io_error](std::string&& msg) {
        if (io_error.empty()) {
            io_error = std::move(msg);
        }
    };

    bioseq::io::reader_options opts = bioseq::io::default_reader_options;
    opts.concurrency.worker_count = 0;
    opts.concurrency.parser_count = 1;
    auto result = bioseq::io::stream_sequences_file<
        p_alphabet, col_unit_t, p_file_format>(
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
template <class p_alphabet, std::size_t p_column_byte_width>
struct file_format_adapter {
    template <bioseq::io::sequence_file_format p_file_format>
    [[gnu::always_inline]] std::int64_t operator()(
            std::integral_constant<
                bioseq::io::sequence_file_format, p_file_format>,
            load_context& ctx,
            pg_identifier_writer& tuple_writer,
            const char* file_path) const {
        return load_records<
            p_alphabet, p_column_byte_width, p_file_format>(
                ctx, tuple_writer, file_path);
    }
};

/** @brief Resolves a runtime file format for one alphabet/width pair. */
template <class p_alphabet, std::size_t p_column_byte_width>
struct format_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    void run() const {
        file_format_adapter<p_alphabet, p_column_byte_width> adapter{};
        out = tpl::dispatch_exact_value_or_default<
            std::int64_t,
            bioseq::io::sequence_file_format,
            bioseq::io::sequence_file_format::fasta>(
                tpl::value_sequence<
                    bioseq::io::sequence_file_format,
                    bioseq::io::sequence_file_format::fasta,
                    bioseq::io::sequence_file_format::fastq>{},
                format, adapter, ctx, tuple_writer, file_path);
    }
};

/** @brief Resolves the encoded storage width for one alphabet. */
template <class p_alphabet>
struct width_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    template <std::size_t p_column_byte_width>
    void operator()() const {
        format_dispatch<p_alphabet, p_column_byte_width>{
            ctx, tuple_writer, file_path, format, out}.run();
    }
};

/** @brief Resolves the encoded sequence alphabet before width dispatch. */
struct alphabet_dispatch {
    load_context& ctx;
    pg_identifier_writer& tuple_writer;
    const char* file_path;
    bioseq::io::sequence_file_format format;
    std::int64_t& out;

    template <class p_alphabet>
    void operator()() const {
        dispatch_storage_unit_byte_width(ctx.cols.byte_width,
            width_dispatch<p_alphabet>{
                ctx, tuple_writer, file_path, format, out});
    }
};
}  // namespace load_dispatch

/** @brief Dispatches the resolved alphabet, storage width, and file format once. */
[[nodiscard]] std::int64_t dispatch_and_stream(
        load_context& ctx, pg_identifier_writer& tuple_writer,
        const char* file_path,
        bioseq::io::sequence_file_kind kind) {
    std::int64_t records_loaded = 0;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(ctx.cols.alphabet_code,
        load_dispatch::alphabet_dispatch{
            ctx, tuple_writer, file_path, kind.format, records_loaded});
    return records_loaded;
}

// --------------------------------------------------------------------------
// Batched index build after the streamed insert
// --------------------------------------------------------------------------

/** @brief Builds update-on-insert indexes after all streamed writes finish. */
void build_indexes_if_enabled(Oid main_relid) {
    if (!bioseq::settings::index::insert_maintenance::enabled) {
        return;
    }
    // The grouped-SoA opt-in deliberately forbids the ordinary per-tuple INSERT
    // hook: that hook owns pre-publication bytes and cannot reopen a committed
    // PostgreSQL locator. bioseq.load is the explicit exception because it
    // first finishes every streamed parent/source write, then reaches this
    // batched SPI bioseq.update_index path. The normal grouped-SoA guards still
    // require both async-persistence GUCs OFF and the authoritative post-load
    // row scan to prove every non-NULL source is locator-backed before clear.
    // bioseq.load is one UDF inside one SQL command. The batched build
    // (build_policy_update_on_insert_indexes → SPI `bioseq.update_index`) issues
    // catalog SPI_execute(read_only=true) calls and columnar reads, both of which
    // require the executor's active snapshot. run_load calls this AFTER
    // finish_load_write_states (so the streamed rows are committed to stripes and
    // read back cleanly) but BEFORE reset_sequence_caches_after_load_storage_alloc
    // (whose DISCARD-SEQUENCES teardown leaves the snapshot unusable — building
    // after it SIGSEGV'd inside SPI_execute). At this point the portal snapshot
    // the SELECT pushed is still on the stack.
    DEBUG_ASSERT(ActiveSnapshotSet(),
                 "bioseq.load: index build needs the portal's active snapshot");
    bioseq::storage::indexing::build_policy_update_on_insert_indexes(main_relid);
}

/** @brief Clears sequence caches invalidated by post-load storage allocation. */
void reset_sequence_caches_after_load_storage_alloc() {
    // Shard allocation advances an internal sequence. Clear its backend-local
    // cache before a same-command index scan and again before returning.
    bioseq::postgres::support::invoke_pg_as_cpp([] { ResetSequenceCaches(); });
}

// --------------------------------------------------------------------------
// Open, stream, flush, close, and index
// --------------------------------------------------------------------------

/** @brief Flushes the current column stripe before post-load index work. */
void flush_pending_load_writes(load_context& ctx) {
    // Parent rows go through ctx.write_state; fragment rows through per-subtable
    // write states opened from ctx.caches. update_index_impl uses raw columnar
    // readers + random reads, so both layers must be flushed before the same UDF
    // starts indexing.
    const SubTransactionId subtransaction =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetCurrentSubTransactionId(); });
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx, subtransaction] {
        flush_relation_write_state(
            ctx.main_relfilenode, subtransaction);
    });
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx, subtransaction] {
        bioseq::storage::flush_seq_cache_write_states(
            ctx.caches, subtransaction);
    });
    bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
}

/** @brief Finalizes parent and sequence-subtable write states exactly once. */
void finish_load_write_states(load_context& ctx) {
    std::vector<Oid> shard_relfilenodes;
    shard_relfilenodes.reserve(ctx.caches == nullptr ? 0 : ctx.caches->entries.size());
    if (ctx.caches != nullptr) {
        for (const auto& kv : ctx.caches->entries) {
            const auto& e = kv.second;
            if (e.rel != nullptr) {
                shard_relfilenodes.push_back(e.rel->rd_locator.relNumber);
            }
        }
    }

    const ResourceOwner saved = CurrentResourceOwner;
    const SubTransactionId subtransaction =
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { return GetCurrentSubTransactionId(); });
    for (Oid relfilenode : shard_relfilenodes) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [saved, relfilenode, subtransaction] {
                CurrentResourceOwner = TopTransactionResourceOwner;
                pop_relation_column_write_state(
                    relfilenode, subtransaction, 0, /*commit=*/true);
                CurrentResourceOwner = saved;
            });
    }
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx, saved, subtransaction] {
        CurrentResourceOwner = TopTransactionResourceOwner;
        pop_relation_column_write_state(
            ctx.main_relfilenode, subtransaction, 0, /*commit=*/true);
        CurrentResourceOwner = saved;
    });

    ctx.write_state = nullptr;
    ctx.caches = nullptr;
    bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
}

// --------------------------------------------------------------------------
// Typed multi-column mapping setup
// --------------------------------------------------------------------------

/** @brief Minimal PostgreSQL type metadata retained for a mapped column. */
struct column_attr {
    std::int16_t attno;
    Oid atttypid;
    std::int32_t atttypmod;
};

/// Decode bioseq.column_mapping[] using the element OID carried by the array.
/// Returned string views remain in the caller's memory context for the load.
[[nodiscard]] std::vector<idp::user_column_mapping>
parse_column_mapping_array(ArrayType* arr) {
    std::vector<idp::user_column_mapping> maps;
    const Oid mapping_type_oid = ARR_ELEMTYPE(arr);
    Datum* elems = nullptr; bool* nulls = nullptr; int nelems = 0;
    bioseq::postgres::support::invoke_pg_as_cpp(
        [arr, mapping_type_oid, &elems, &nulls, &nelems] {
        deconstruct_array(
            arr, mapping_type_oid, -1, false, 'd',
            &elems, &nulls, &nelems);
    });
    TupleDesc cmdesc = bioseq::postgres::support::invoke_pg_as_cpp(
        [mapping_type_oid] {
            return lookup_rowtype_tupdesc(mapping_type_oid, -1);
        });
    const MemoryContext mapping_result_context = CurrentMemoryContext;
    std::exception_ptr failure;
    try {
        column_mapping_codec::validate_descriptor(cmdesc);
        for (int i = 0; i < nelems; ++i) {
            if (nulls[i]) {
                raise_load_error_as_cpp(
                    ERRCODE_NULL_VALUE_NOT_ALLOWED,
                    fmt::format("columns[] element {} is NULL", i + 1));
            }
            HeapTupleHeader th =
                bioseq::postgres::support::pg_datum::to_heap_tuple_header(
                    bioseq::postgres::support::pg_datum::from_raw(elems[i]));
            HeapTupleData tmp{};
            tmp.t_len  = HeapTupleHeaderGetDatumLength(th);
            tmp.t_data = th;
            const column_mapping_row mapping =
                column_mapping_codec::deform(
                    &tmp,
                    cmdesc,
                    mapping_result_context);
            if (!mapping.column_name.has_value()
                || *mapping.column_name == nullptr) {
                raise_load_error_as_cpp(
                    ERRCODE_NULL_VALUE_NOT_ALLOWED,
                    fmt::format(
                        "columns[{}].column_name is NULL", i + 1));
            }
            const char* col = bioseq::postgres::support::invoke_pg_as_cpp(
                [value = *mapping.column_name] {
                    return text_to_cstring(value);
                });
            const char* ds =
                !mapping.data_source.has_value()
                    || *mapping.data_source == nullptr
                ? ""
                : bioseq::postgres::support::invoke_pg_as_cpp(
                    [value = *mapping.data_source] {
                        return text_to_cstring(value);
                    });
            maps.push_back(idp::user_column_mapping{
                std::string_view{col}, std::string_view{ds}});
        }
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [cmdesc] { ReleaseTupleDesc(cmdesc); });
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    return maps;
}

/// Resolve column mappings and prepare coercion, defaults, and constraints
/// before streaming.
void setup_typed_column_mapping(
        load_context& ctx, const char* path, bioseq::io::sequence_file_kind kind,
        idp::header_format requested_fmt,
        std::span<const idp::user_column_mapping> user_maps) {
    ctx.requested_fmt = requested_fmt;
    const TupleDesc td = ctx.parent_td;
    const std::int16_t seq_attno = ctx.cols.seq_attno;

    // Record a single-column primary key; composite keys need no special path.
    std::int16_t pk_attno = 0;
    {
        const Oid pk_oid = bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] { return bioseq::postgres::support::find_primary_key_oid(ctx.parent_rel); });
        if (OidIsValid(pk_oid)) {
            Relation pk_idx = bioseq::postgres::support::invoke_pg_as_cpp(
                [pk_oid] { return index_open(pk_oid, AccessShareLock); });
            if (pk_idx->rd_index != nullptr
                && pk_idx->rd_index->indnatts == 1) {
                pk_attno = static_cast<std::int16_t>(
                    pk_idx->rd_index->indkey.values[0]);
            }
            bioseq::postgres::support::invoke_pg_as_cpp(
                [pk_idx] { index_close(pk_idx, AccessShareLock); });
        }
    }

    // Flatten non-sequence columns without leaking PostgreSQL types into the
    // identifier resolver. Identity and generated columns are unsupported.
    std::vector<idp::column_descriptor> columns;
    std::vector<column_attr> col_attrs;
    for (int i = 0; i < td->natts; ++i) {
        const Form_pg_attribute attr = TupleDescAttr(td, i);
        if (attr->attisdropped || attr->attnum <= 0) {
            continue;
        }
        const std::int16_t attno = static_cast<std::int16_t>(i + 1);
        if (attno == seq_attno) {
            continue;
        }
        if (attr->attidentity != '\0' || attr->attgenerated != '\0') {
            raise_load_error_as_cpp(
                ERRCODE_INVALID_TABLE_DEFINITION,
                fmt::format(
                    "column \"{}\" is an identity or generated column; "
                    "bioseq.load cannot populate identity/generated columns",
                    NameStr(attr->attname)));
        }
        const bool is_text = (attr->atttypid == TEXTOID ||
                              attr->atttypid == VARCHAROID ||
                              attr->atttypid == BPCHAROID);
        columns.push_back(idp::column_descriptor{
            .name = std::string_view{NameStr(attr->attname)},
            .is_text = is_text,
            .is_primary_key = (pk_attno != 0 && attno == pk_attno),
            .not_null = attr->attnotnull,
            .has_default = (attr->atthasdef != false),
            .is_identity_or_generated = false,
        });
        col_attrs.push_back(column_attr{attno, attr->atttypid, attr->atttypmod});
    }

    // Preserve the whole header when AUTO has one text destination and no
    // explicit mapping.
    if (requested_fmt == idp::header_format::automatic && user_maps.empty() &&
        columns.size() == 1 && columns[0].is_text) {
        ctx.parser_active = false;
        ctx.effective_fmt = idp::header_format::plain;
        ctx.cols.header_attno = col_attrs[0].attno;
        return;
    }
    ctx.parser_active = true;

    // Sample headers to determine the effective AUTO format.
    idp::header_format effective_fmt = requested_fmt;
    if (requested_fmt == idp::header_format::automatic) {
        const auto samples = bioseq::io::sample_sequence_headers(
            path, kind.format, bioseq::io::default_reader_options);
        std::vector<std::string_view> views;
        if (samples) {
            for (const std::string& sample : *samples) {
                views.emplace_back(sample);
            }
        }
        effective_fmt = idp::detect_header_format(views);
        if (effective_fmt == idp::header_format::plain) {
            const bool has_metadata_target =
                columns.size() > 1 ||
                (columns.size() == 1 && !columns[0].is_text) ||
                !user_maps.empty();
            if (has_metadata_target) {
                effective_fmt = idp::header_format::automatic;
            }
        }
    }
    ctx.effective_fmt = effective_fmt;

    // Resolve the PostgreSQL-independent identifier mapping.
    auto map = idp::resolve_identifier_mapping(
        requested_fmt, effective_fmt, columns, user_maps);
    if (!map) {
        raise_load_error_as_cpp(
            ERRCODE_INVALID_TABLE_DEFINITION, map.error());
    }
    ctx.routing = std::move(map->routing);
    const std::vector<std::uint16_t> slot_to_column = std::move(map->slot_to_column);

    // Reject an unmapped NOT NULL column that has no default. Per-row NULL
    // violations are left to ExecConstraints.
    std::vector<char> referenced(columns.size(), 0);
    for (const std::uint16_t column_index : slot_to_column) {
        referenced[column_index] = 1;
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (referenced[i]) {
            continue;
        }
        if (columns[i].not_null && !columns[i].has_default) {
            raise_load_error_as_cpp(
                ERRCODE_INVALID_TABLE_DEFINITION,
                fmt::format(
                    "column \"{}\" is NOT NULL but is unmapped and has no default",
                    columns[i].name));
        }
    }

    // Build one cached coercion entry per resolved destination.
    ctx.dest.clear();
    ctx.dest.reserve(slot_to_column.size());
    for (const std::uint16_t ci : slot_to_column) {
        const column_attr& ca = col_attrs[ci];
        Oid input_fn = InvalidOid;
        Oid io_param = InvalidOid;
        char typtype = '\0';
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&ca, &input_fn, &io_param, &typtype] {
                getTypeInputInfo(ca.atttypid, &input_fn, &io_param);
                typtype = get_typtype(getBaseType(ca.atttypid));
            });
        dest_kind kind;
        if (columns[ci].is_text)
            kind = ca.atttypmod < 0
                ? dest_kind::text
                : dest_kind::text_with_typmod;
        else if (typtype == TYPTYPE_RANGE)
            kind = dest_kind::range;
        else if (typtype == TYPTYPE_MULTIRANGE)
            kind = dest_kind::multirange;
        else
            kind = dest_kind::scalar;
        ctx.dest.push_back(dest_coercion{
            .attno = ca.attno,
            .column_name = std::string{columns[ci].name},
            .kind = kind,
            .input_fn = input_fn,
            .io_param = io_param,
            .typmod = ca.atttypmod,
        });
    }

    // Compile defaults as executor expressions so volatile defaults evaluate
    // once per row.
    ctx.defaults.clear();
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (!columns[i].has_default) {
            continue;
        }
        Node* const raw = bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx, &col_attrs, i] {
                return build_column_default(
                    ctx.parent_rel, col_attrs[i].attno);
            });
        if (raw == nullptr) {
            continue;
        }
        Expr* const planned = bioseq::postgres::support::invoke_pg_as_cpp([raw] {
            return expression_planner(reinterpret_cast<Expr*>(raw));
        });
        ExprState* const state = bioseq::postgres::support::invoke_pg_as_cpp(
            [planned] { return ExecInitExpr(planned, nullptr); });
        ctx.defaults.push_back(default_col{
            col_attrs[i].attno, state});
    }

    // A private executor is needed only when PostgreSQL must evaluate a default
    // or enforce a relation constraint. Header parsing/coercion alone does not
    // need an EState. Besides avoiding unnecessary executor setup on the hot
    // load path, this keeps a nested AfterTrigger query frame out of ordinary
    // typed loads before their same-statement SPI index build.
    const bool needs_private_executor = !ctx.defaults.empty()
        || ctx.parent_rel->rd_att->constr != nullptr;
    if (!needs_private_executor) {
        return;
    }
    ctx.estate = bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] {
            return bioseq::storage::metadata::tuple_ops::
                create_relation_executor_state(ctx.parent_rel);
        });
    ctx.econtext = bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { return GetPerTupleExprContext(ctx.estate); });
    ctx.result_rel_info = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return makeNode(ResultRelInfo); });
    bioseq::postgres::support::invoke_pg_as_cpp([&ctx] {
        InitResultRelInfo(
            ctx.result_rel_info, ctx.parent_rel, 1, nullptr, 0);
    });
    // NB: NO ExecOpenIndices here — this path only restores NOT NULL / CHECK
    // enforcement around write_column_row; index maintenance stays on the
    // existing Bioseq load/index policy path.
}

/// Tear down the optional per-load executor state. This is idempotent.
void teardown_load_executor(load_context& ctx, MemoryContext stable_context) {
	if (ctx.estate == nullptr) {
        return;
    }
	// FreeExecutorState destroys the estate's executor contexts.  The checked
	// PostgreSQL boundary restores the context that was current on entry, so it
	// must enter from a caller-owned context that survives the deletion.
	MemoryContextSwitchTo(stable_context);
	bioseq::postgres::support::invoke_pg_as_cpp(
		[&ctx] { AfterTriggerEndQuery(ctx.estate); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { ExecCloseResultRelations(ctx.estate); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { ExecCloseRangeTableRelations(ctx.estate); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { ExecResetTupleTable(ctx.estate->es_tupleTable, false); });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [&ctx] { FreeExecutorState(ctx.estate); });
    ctx.estate = nullptr;
    ctx.econtext = nullptr;
    ctx.result_rel_info = nullptr;
}

/** @brief Releases invocation-owned executor, slot, and parent-relation state. */
void cleanup_load_context(
		load_context& ctx, bool commit, MemoryContext stable_context) {
	MemoryContextSwitchTo(stable_context);
	// Release the per-load executor state before
	// closing the parent relation. Idempotent (no-op if already torn down).
	teardown_load_executor(ctx, stable_context);
    if (ctx.slot != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] { ExecDropSingleTupleTableSlot(ctx.slot); });
        ctx.slot = nullptr;
    }
    // Write-state teardown is explicit in finish_load_write_states(). This
    // cleanup owns only the slot and parent relation.
    (void)commit;
    ctx.caches = nullptr;
    const ResourceOwner saved = CurrentResourceOwner;
    if (ctx.parent_rel != nullptr) {
        Relation relation = ctx.parent_rel;
        bioseq::postgres::support::invoke_pg_as_cpp([relation, saved] {
            CurrentResourceOwner = TopTransactionResourceOwner;
            table_close(relation, NoLock);
            CurrentResourceOwner = saved;
        });
        ctx.parent_rel = nullptr;
    }
}

/** @brief Owns the complete streamed-load lifecycle and converges cleanup. */
[[nodiscard]] std::int64_t run_load(
        Oid relid, const char* path, bioseq::io::sequence_file_kind kind,
        idp::header_format requested_fmt,
        std::span<const idp::user_column_mapping> user_maps) {
	load_context ctx{};
	ctx.main_relid = relid;
	const MemoryContext stable_context = CurrentMemoryContext;
	std::int64_t n = 0;
    std::exception_ptr failure;

    try {
        // Open the parent under TopTransactionResourceOwner, then restore the
        // caller's (executor/Portal) owner for the stream itself so the slot +
        // tuple descriptors stay owned by the Portal. Fragment shards are
        // similarly pinned by sequence_stream_writer.
        const ResourceOwner saved = CurrentResourceOwner;
        ctx.parent_rel = bioseq::postgres::support::invoke_pg_as_cpp([relid, saved] {
            CurrentResourceOwner = TopTransactionResourceOwner;
            Relation result = table_open(relid, RowExclusiveLock);
            CurrentResourceOwner = saved;
            return result;
        });

        ctx.parent_td = RelationGetDescr(ctx.parent_rel);
        ctx.cols = resolve_load_columns(ctx.parent_rel);
        setup_typed_column_mapping(
            ctx, path, kind, requested_fmt, user_maps);
        ctx.db_oid = ctx.parent_rel->rd_locator.dbOid;
        ctx.spc_oid = ctx.parent_rel->rd_locator.spcOid;
        ctx.main_relfilenode = ctx.parent_rel->rd_locator.relNumber;
        ctx.main_storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] {
                return column_storage_id(
                    ctx.parent_rel, false);
            });
        ctx.cap_bytes = bioseq::storage::read_max_sub_table_cap_bytes();

        const SubTransactionId subtransaction =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentSubTransactionId(); });
        ctx.write_state = initialize_column_write_state(
            ctx.parent_rel, ctx.parent_td, relid, subtransaction);
        if (ctx.write_state->seq_caches == nullptr) {
            auto* c = bioseq::postgres::support::invoke_pg_as_cpp([&ctx] {
                return bioseq::storage::alloc_seq_caches(
                    MemoryContextGetParent(
                        ctx.write_state->stripe_write_context));
            });
            c->side                = bioseq::storage::seq_caches::access_side::write;
            c->lock_mode           = RowExclusiveLock;
            c->main_database_oid   = ctx.db_oid;
            c->main_tablespace_oid = ctx.spc_oid;
            c->main_relfilenode    = ctx.main_relfilenode;
            c->cap_bytes_snapshot  = ctx.cap_bytes;
            ctx.write_state->seq_caches = c;
        }
        ctx.caches = static_cast<bioseq::storage::seq_caches*>(
            ctx.write_state->seq_caches);
        ctx.slot = bioseq::postgres::support::invoke_pg_as_cpp(
            [&ctx] {
                return MakeTupleTableSlot(ctx.parent_td, &TTSOpsVirtual);
            });

        pg_identifier_writer writer{ctx, ctx.dest, ctx.defaults};
        ctx.writer_ref = idp::make_identifier_writer_reference(writer);
        n = dispatch_and_stream(ctx, writer, path, kind);
		teardown_load_executor(ctx, stable_context);
		flush_pending_load_writes(ctx);
		MemoryContextSwitchTo(stable_context);
		finish_load_write_states(ctx);
        build_indexes_if_enabled(ctx.main_relid);
        reset_sequence_caches_after_load_storage_alloc();
    } catch (...) {
        failure = std::current_exception();
    }

	try {
		cleanup_load_context(ctx, failure == nullptr, stable_context);
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }

    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    return n;
}

}  // anonymous namespace

export namespace bioseq::storage::load_pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * @brief Loads FASTA or FASTQ records into a Bioseq table.
 */
[[nodiscard]] std::int64_t load(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::optional<Oid> target_table,
        std::optional<std::string> file_path,
        [[bioseq_sql::param({
            .default_value = bioseq::sql::checked_sql{"'auto'"},
        })]]
        std::optional<std::string> header_format,
        [[bioseq_sql::param({
            .sql_type =
                bioseq::sql::custom_type<"bioseq.column_mapping[]">,
            .default_value = bioseq::sql::checked_sql{"NULL"},
        })]]
        std::optional<ArrayType*> columns) {
    if (!target_table.has_value() || !file_path.has_value()) {
        raise_load_error_as_cpp(
            ERRCODE_NULL_VALUE_NOT_ALLOWED,
            "target_table and file_path must not be NULL");
    }
    const Oid relid = *target_table;
    const char* const path = file_path->c_str();
    const char* const fmt_arg =
        header_format.has_value() ? header_format->c_str() : "auto";

    // 1. file kind (NO_POSTGRES) — error -> ereport.
    auto kind_or = bioseq::io::detect_sequence_file_kind(path);
    if (!kind_or) {
        raise_load_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE, kind_or.error());
    }
    const auto kind = *kind_or;

    // 2. header_format name -> enum (NO_POSTGRES).
    auto fmt_or = bioseq::io::identifier_parser::parse_header_format_name(fmt_arg);
    if (!fmt_or) {
        raise_load_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE, fmt_or.error());
    }
    const bioseq::io::identifier_parser::header_format requested_fmt = *fmt_or;

    // 3. columns => bioseq.column_mapping[] (may be NULL).
    std::vector<bioseq::io::identifier_parser::user_column_mapping> user_maps;
    if (columns.has_value()) {
        user_maps = parse_column_mapping_array(*columns);
    }

    // 4..14 — open, discover columns, resolve mapping, stream, build index, close.
    const std::int64_t n = run_load(relid, path, kind, requested_fmt, user_maps);
    bioseq_log<logging_level::debug>("[bioseq] load: '{}' -> relid={} loaded {} records", path, relid, n);
    return n;
}

}  // namespace bioseq::storage::load_pg::sql_routines
