// bioseq/backend/storage/io/load.cppm
//
// DOC0286 §4 — the postgres `bioseq.load(target_table, file_path)` UDF. Streams
// a FASTA/FASTQ file through the postgres-agnostic IO reader
// (`stream_sequences_file`) into a USING-bioseq table, byte-slice-only (no
// >MaxAllocSize palloc). Runtime (alphabet_code, byte_width, file_type) are
// fanned out to compile-time types via the existing alphabet/width dispatchers
// plus `bioseq_common.templating::enum_dispatch_pack` — never an if-ladder.
module;
// DOC0145 / DOC0192 §6 — common_simd.h forwards <hwy/highway.h> and MUST precede
// common.h: load.cppm imports two HWY-bearing module graphs (the IO reader via
// bioseq_io.public_api AND bioseq_sequences.simd via pg_compact_sequence), and
// without owning the Highway intrinsics in this TU their arm_neon.h static
// definitions collide ("duplicate mangled name _ZL...vdupq_n_u8").
#include "common_simd.h"
#include "common.h"            // DEBUG_ASSERT, JustLog, CXX_PG_FUNCTION_INFO_V1, fmt
#include <boost/integer.hpp>
#include <boost/container/flat_map.hpp>   // identifier_routing containers (DOC0295 §4.3)
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
#include "access/relation.h"   // index_open / index_close (DOC0295 §7 step 4 PK probe)
#include "catalog/namespace.h" // (parity w/ sample_table_udf; relid arrives as regclass)
#include "catalog/pg_type_d.h" // TEXTOID, VARCHAROID, BPCHAROID
#include "commands/sequence.h" // ResetSequenceCaches
#include "executor/tuptable.h" // TupleTableSlot, MakeTupleTableSlot, TTSOpsVirtual, Exec*Tuple
#include "executor/executor.h" // ExecConstraints, ExecInitExpr, ExecEvalExpr, InitResultRelInfo (DOC0295 §9.3)
#include "nodes/makefuncs.h"   // makeNode (ResultRelInfo)
#include "rewrite/rewriteHandler.h" // build_column_default (DOC0295 §9.3)
#include "optimizer/optimizer.h"    // expression_planner
#include "utils/builtins.h"    // text_to_cstring, cstring_to_text_with_len, regtypein
#include "utils/lsyscache.h"   // getTypeInputInfo, get_typtype, getBaseType (DOC0295 §7/§9.1)
#include "utils/typcache.h"    // lookup_rowtype_tupdesc (DOC0295 §9.2)
#include "utils/array.h"       // deconstruct_array, ARR_ELEMTYPE (DOC0295 §9.2)
#include "utils/rel.h"         // RelationGetDescr
#include "utils/resowner.h"    // CurrentResourceOwner, TopTransactionResourceOwner
#include "utils/snapmgr.h"     // PushActiveSnapshot/PopActiveSnapshot/GetTransactionSnapshot, ActiveSnapshotSet
}
#include "columnar/columnar.h" // bioseq_update_index_on_insert +
                               // FlushWriteStateForRelfilenode declarations
export module bioseq_storage_io_load;

import mlogging;                           // JustLog (enable_logging)
import pg_utils;
import bioseq_storage_size;                // sequence_size_t
import bioseq_common.templating;           // enum_dispatch_pack / value_sequence
import bioseq_sequences;                   // compact_sequence_stdc_type_params::decode,
                                           //   dispatch_storage_unit_byte_width, traits
import bioseq_types.pg_compact_sequence;   // pgseq_t, pgseq_supported_alphabets
import bioseq_storage;                     // alloc_locator_stub_in, alloc_seq_caches, seq_caches,
                                           //   read_max_sub_table_cap_bytes,
                                           //   kSequenceFragmentStorageUnits, sequence_stream_writer,
                                           //   flush_seq_cache_write_states, seq_locator
import bioseq_write_state_management;      // bioseq_init_write_state
import bioseq_table_io;                    // ColumnarWriteRow, ColumnarWritePerTupleContext
import bioseq_storage_indexing_admin;      // build_policy_update_on_insert_indexes
import bioseq_io.options;                  // sequence_io_file_type, reader_options
import bioseq_io.file_utils;               // infer_sequence_file_kind, sequence_file_kind
import bioseq_io.public_api;               // stream_sequences_file, packed_sequence_chunk,
                                           //   sample_sequence_headers
import bioseq_io.identifier_formats;       // header_format, header_format_from_sql_name (DOC0295)
import bioseq_io.identifier_parser;        // resolve_identifier_mapping, parse_and_route_header
import storage_metadata;                   // create_estate_for_relation (DOC0295 §9.3)
                                           // (pg_utils already imported above: find_primary_key_oid)
import bioseq_parallelism.mode;
import bioseq_parallelism.concepts;
import bioseq_parallelism.pool;
import bioseq_scheduler.supervisor_pg;

extern "C++" void ColumnarPopWriteStateForRelfilenode(
    Oid relfilenode, SubTransactionId current_sub_xid,
    SubTransactionId parent_sub_xid, bool commit);

namespace {

using bioseq::storage::sequence_size_t;   // bp counts (uint64 — 6 Gbp safe)

// --------------------------------------------------------------------------
// §4.1 — column discovery
// --------------------------------------------------------------------------

[[nodiscard]] Oid load_seq_type_oid() {
    return DatumGetObjectId(DirectFunctionCall1(
        regtypein, CStringGetDatum("bioseq.seq")));
}

struct LoadColumns {
    std::int16_t header_attno;   // 0 = none (header discarded)
    std::int16_t seq_attno;      // the one bioseq.seq column (always required)
    std::uint8_t alphabet_code;  // from the seq column's typmod
    std::uint8_t byte_width;     // 1/2/4/8, from the seq column's typmod
};

// DOC0295 §3 / §7 step 3 — discover the single bioseq.seq column and decode
// (alphabet, width) from its typmod. The multi-text-column and NOT-NULL/default/
// identity/generated rules are now handled by the identifier-parser mapping
// resolution + the per-column descriptor build (build_load_columns), NOT here:
// the loader supports many typed metadata columns now (DOC0295). header_attno is
// only used by the legacy whole-header passthrough (parser_active=false).
[[nodiscard]] LoadColumns resolve_load_columns(Relation rel) {
    TupleDesc td = RelationGetDescr(rel);
    const Oid seq_type_oid = load_seq_type_oid();
    if (!OidIsValid(seq_type_oid))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
            errmsg("bioseq.load: bioseq.seq type not found (is the extension installed?)")));

    std::int16_t header_attno = 0, seq_attno = 0;
    int seq_count = 0;
    for (int i = 0; i < td->natts; ++i) {
        Form_pg_attribute attr = TupleDescAttr(td, i);
        if (attr->attisdropped || attr->attnum <= 0) continue;
        if (attr->atttypid == seq_type_oid) {
            ++seq_count; seq_attno = static_cast<std::int16_t>(i + 1);
        } else if ((attr->atttypid == TEXTOID || attr->atttypid == VARCHAROID
                   || attr->atttypid == BPCHAROID) && header_attno == 0) {
            header_attno = static_cast<std::int16_t>(i + 1);  // first text column
        }
    }
    // Exactly one seq column (unchanged).
    if (seq_count == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
            errmsg("bioseq.load: table has no bioseq.seq column")));
    if (seq_count > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
            errmsg("bioseq.load: table has %d bioseq.seq columns; exactly one is "
                   "required (v1)", seq_count)));

    // (alphabet, width) off the seq column's typmod — NEVER hardcoded.
    Form_pg_attribute seq_attr = TupleDescAttr(td, seq_attno - 1);
    if (seq_attr->atttypmod < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
            errmsg("bioseq.load: bioseq.seq column has no typmod (declare it as "
                   "bioseq.seq('<alphabet>', <width>))")));
    const auto tp = compact_sequence_stdc_type_params::decode(seq_attr->atttypmod);
    return LoadColumns{header_attno, seq_attno, tp.alphabet_code,
                       tp.storage_unit_byte_width};
}

// --------------------------------------------------------------------------
// §9.1 — typed-column coercion table + the concept-satisfying PG writer
// --------------------------------------------------------------------------
namespace idp = bioseq::io::idparse;

// TEXT_TYPMOD = varchar(n)/bpchar(n) with a length constraint -> must run the
// input function so the length is enforced (NOT the raw cstring_to_text path).
enum class dest_kind : std::uint8_t { TEXT, TEXT_TYPMOD, SCALAR, RANGE, MULTIRANGE };

struct dest_coercion {
    std::int16_t attno;        // 1-based
    std::string  column_name;  // copied once for the ErrorContextCallback
    dest_kind    kind;
    Oid          input_fn;     // from getTypeInputInfo
    Oid          io_param;     // typioparam (enum type OID for enums)
    std::int32_t typmod;       // column atttypmod (varchar(n), numeric(p,s), …)
};

struct default_col { std::int16_t attno; ExprState* state; };

/// 2-byte substring scan; the repo has no `memmem` user — do not rely on the
/// POSIX symbol (DOC0295 §9.1).
[[nodiscard]] const char* find_substr(const char* data, int len,
                                      const char* needle) {
    const int nlen = static_cast<int>(std::strlen(needle));
    if (nlen == 0 || len < nlen) return nullptr;
    for (int i = 0; i + nlen <= len; ++i)
        if (std::memcmp(data + i, needle, static_cast<std::size_t>(nlen)) == 0)
            return data + i;
    return nullptr;
}

/// Copy `[data,len)` into a per-tuple-palloc'd NUL-terminated cstring. For a
/// RANGE/MULTIRANGE column with exactly one `..`, rewrite `lo..hi` to the
/// canonical inclusive `[lo,hi]` (multirange: `{[lo,hi]}`); otherwise the trimmed
/// text is passed straight to `<type>range_in`/`<type>multirange_in` (E14/F16).
[[nodiscard]] char* make_cstr(const char* data, int len,
                              const dest_coercion& dc, MemoryContext mctx) {
    while (len > 0 && data[0] == ' ') { ++data; --len; }
    while (len > 0 && data[len - 1] == ' ') { --len; }
    const bool is_range = (dc.kind == dest_kind::RANGE ||
                           dc.kind == dest_kind::MULTIRANGE);
    if (is_range) {
        const char* d0 = find_substr(data, len, "..");
        const bool one_dots = d0 &&
            !find_substr(d0 + 2, len - static_cast<int>(d0 - data) - 2, "..");
        if (d0 && one_dots) {
            const int la = static_cast<int>(d0 - data);
            const int hb_off = la + 2;
            const int hb = len - hb_off;
            const char* lo = data; int lol = la;
            while (lol && lo[0] == ' ') { ++lo; --lol; }
            while (lol && lo[lol - 1] == ' ') --lol;
            const char* hi = data + hb_off; int hil = hb;
            while (hil && hi[0] == ' ') { ++hi; --hil; }
            while (hil && hi[hil - 1] == ' ') --hil;
            const bool mr = (dc.kind == dest_kind::MULTIRANGE);
            char* out = static_cast<char*>(MemoryContextAlloc(
                mctx, static_cast<std::size_t>(lol + hil + 6)));
            int k = 0;
            if (mr) out[k++] = '{';
            out[k++] = '[';
            std::memcpy(out + k, lo, static_cast<std::size_t>(lol)); k += lol;
            out[k++] = ',';
            std::memcpy(out + k, hi, static_cast<std::size_t>(hil)); k += hil;
            out[k++] = ']';
            if (mr) out[k++] = '}';
            out[k] = '\0';
            return out;
        }
        // 0 or >1 ".." -> hand the trimmed text to <type>range_in/<type>multirange_in.
    }
    char* out = static_cast<char*>(MemoryContextAlloc(
        mctx, static_cast<std::size_t>(len) + 1));
    std::memcpy(out, data, static_cast<std::size_t>(len));
    out[len] = '\0';
    return out;
}

// --------------------------------------------------------------------------
// §4.4 / §9.1 — load context
// --------------------------------------------------------------------------

struct pg_identifier_writer;  // fwd (holds LoadContext&; defined below)

struct LoadContext {
    Oid main_relid;                                  // parent table OID (for update_index)
    Oid db_oid, spc_oid; RelFileNumber main_relfilenode;
    Relation parent_rel; TupleDesc parent_td;
    LoadColumns cols;
    bioseq::storage::seq_caches* caches;
    ColumnarWriteState* write_state;
    sequence_size_t cap_bytes;
    TupleTableSlot* slot;

    // --- DOC0295: identifier parser + typed multi-column mapping ---
    // LOAD-BEARING TEMPLATE-EXPLOSION INVARIANT (§3/§4.4): write_parent_row stays
    // NON-templated and pg_identifier_writer is ONE concrete type that does NOT
    // capture the alphabet/width — so parse_and_route_header is one runtime call
    // and adds ZERO entries to the (alphabet × width × file_type) = 16 fan-out.
    bool                       parser_active = false;
    idp::header_format         requested_fmt = idp::header_format::AUTO;
    idp::header_format         effective_fmt = idp::header_format::AUTO;
    idp::identifier_routing    routing;
    idp::identifier_parse_state parse_state;
    pg_identifier_writer*      writer = nullptr;       // borrowed; lives in run_load
    idp::identifier_writer_ref writer_ref;
    std::vector<dest_coercion> dest;                   // index == slot
    std::vector<default_col>   defaults;
    EState*                    estate = nullptr;
    ExprContext*               econtext = nullptr;
    ResultRelInfo*             result_rel_info = nullptr;

    // Read only by the per-record ErrorContextCallback (§10).
    std::uint64_t              record_no = 0;
    std::string_view           current_header;
    const char*                current_column_name = nullptr;
};

/// §10 — per-record error context. Postgres appends this to a coercion / default
/// / constraint failure's own message. The header is truncated to a bounded
/// prefix so a malformed giant header cannot make the message unbounded.
void bioseq_load_error_context(void* arg) {
    auto* ctx = static_cast<LoadContext*>(arg);
    const std::string_view h = ctx->current_header;
    const int hlen = static_cast<int>(std::min<std::size_t>(h.size(), 240));
    if (ctx->current_column_name)
        errcontext("bioseq.load: record %llu, column \"%s\", header \"%.*s\"",
                   static_cast<unsigned long long>(ctx->record_no),
                   ctx->current_column_name, hlen, h.data());
    else
        errcontext("bioseq.load: record %llu, header \"%.*s\"",
                   static_cast<unsigned long long>(ctx->record_no), hlen, h.data());
}

/// §9.1 — the concept-satisfying PG sink. Owns the coercion table + per-tuple
/// slot; routes each raw byte range through its column's cached input function.
struct pg_identifier_writer {
    LoadContext&                   ctx;
    std::span<const dest_coercion> dest;       // index == slot
    std::span<const default_col>   defaults;   // unmapped/unset columns w/ default

    void begin_tuple() {
        ExecClearTuple(ctx.slot);
        const int natts = ctx.parent_td->natts;
        for (int i = 0; i < natts; ++i) ctx.slot->tts_isnull[i] = true;
    }
    void set_null(idp::dest_slot_t slot) {
        ctx.slot->tts_isnull[dest[slot].attno - 1] = true;
    }
    void set_field(idp::dest_slot_t slot, const char* data, std::int32_t len) {
        const dest_coercion& dc = dest[slot];
        ctx.current_column_name = dc.column_name.c_str();   // errcontext if input fn longjmps
        MemoryContext per_tuple = ColumnarWritePerTupleContext(ctx.write_state);
        MemoryContext old = MemoryContextSwitchTo(per_tuple);
        Datum v;
        if (dc.kind == dest_kind::TEXT) {                   // plain text/varchar/bpchar, no typmod
            v = PointerGetDatum(cstring_to_text_with_len(data, len));
        } else {                                            // TEXT_TYPMOD/SCALAR/RANGE/MULTIRANGE
            char* cstr = make_cstr(data, len, dc, per_tuple);
            v = OidInputFunctionCall(dc.input_fn, cstr, dc.io_param, dc.typmod);
        }
        MemoryContextSwitchTo(old);
        ctx.slot->tts_values[dc.attno - 1] = v;
        ctx.slot->tts_isnull[dc.attno - 1] = false;
        ctx.current_column_name = nullptr;
    }
    void end_tuple() {
        // Defaults fill ONLY columns the parser left NULL — a mapped value always
        // wins over a column default (§9.3). by-ref default datums live in the
        // econtext memory and stay valid until ColumnarWriteRow copies them.
        for (const default_col& d : defaults) {
            if (!ctx.slot->tts_isnull[d.attno - 1]) continue;
            bool isnull = false;
            Datum v = ExecEvalExpr(d.state, ctx.econtext, &isnull);
            ctx.slot->tts_values[d.attno - 1] = v;
            ctx.slot->tts_isnull[d.attno - 1] = isnull;
        }
        ExecStoreVirtualTuple(ctx.slot);
        if (ctx.result_rel_info && ctx.parent_rel->rd_att->constr)
            ExecConstraints(ctx.result_rel_info, ctx.slot, ctx.estate);
        (void) ColumnarWriteRow(ctx.write_state, ctx.slot->tts_values,
                                ctx.slot->tts_isnull);
        // NEW-2: reset temporary contexts ONLY AFTER ColumnarWriteRow has copied
        // the by-ref datums — resetting earlier is a use-after-free (CRITICAL-04).
        if (ctx.econtext) ResetExprContext(ctx.econtext);
        MemoryContextReset(ColumnarWritePerTupleContext(ctx.write_state));
    }
    void finish() {}
    [[nodiscard]] std::size_t dest_count() const { return dest.size(); }
};
static_assert(idp::is_identifier_dest_writer<pg_identifier_writer>);

// write_parent_row stays a NON-templated free function (template-explosion
// invariant, §4.4): it drives the one concrete pg_identifier_writer via the
// type-erased ref, so the parser is never multiplied by (alphabet,width,file_type).
void write_parent_row(LoadContext& ctx, std::string_view header,
                      std::uint8_t alphabet_code, std::uint8_t byte_width,
                      std::uint16_t last_pad, sequence_size_t total_bp,
                      const bioseq::storage::seq_locator& locator) {
    pgseq_t* stub = bioseq::storage::alloc_locator_stub_in(
        ColumnarWritePerTupleContext(ctx.write_state),
        alphabet_code, byte_width, last_pad, total_bp, locator);

    ++ctx.record_no;
    ctx.current_header = header;
    ctx.current_column_name = nullptr;
    ErrorContextCallback errctx;
    errctx.callback = bioseq_load_error_context;
    errctx.arg = &ctx;
    errctx.previous = error_context_stack;
    error_context_stack = &errctx;

    ctx.writer->begin_tuple();                              // ExecClearTuple + all-NULL
    ctx.slot->tts_values[ctx.cols.seq_attno - 1] = PointerGetDatum(stub);
    ctx.slot->tts_isnull[ctx.cols.seq_attno - 1] = false;

    if (ctx.parser_active) {
        auto routed = idp::parse_and_route_header(
            ctx.effective_fmt, header, ctx.routing, ctx.parse_state, ctx.writer_ref);
        if (!routed)
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("bioseq.load: %s", routed.error().c_str())));
    } else if (ctx.cols.header_attno != 0) {               // D7 passthrough: whole header verbatim
        if (header.size() > static_cast<std::size_t>(INT_MAX))
            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("bioseq.load: FASTA/FASTQ header exceeds INT_MAX")));
        MemoryContext old = MemoryContextSwitchTo(
            ColumnarWritePerTupleContext(ctx.write_state));
        text* htext = cstring_to_text_with_len(header.data(),
                                               static_cast<int>(header.size()));
        MemoryContextSwitchTo(old);
        ctx.slot->tts_values[ctx.cols.header_attno - 1] = PointerGetDatum(htext);
        ctx.slot->tts_isnull[ctx.cols.header_attno - 1] = false;
    }
    ctx.writer->end_tuple();   // defaults, ExecConstraints, ColumnarWriteRow, ctx reset
    error_context_stack = errctx.previous;
    ctx.current_header = {};
    ctx.current_column_name = nullptr;
}

// --------------------------------------------------------------------------
// §4.3 — bounded packed streaming sink + load_records
// --------------------------------------------------------------------------

template <sequence_alphabet A, std::size_t ColBW>
struct load_stream_sink {
    using col_cfg = sequence_custom_alphabet<A,
                       typename boost::uint_t<ColBW * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;
    static constexpr std::size_t col_lpu =
        compact_sequence_range<col_cfg>::traits::letters_per_unit;
    static constexpr std::uint8_t kColAlphabetCode =
        pgseq_supported_alphabets::index_of<A>();

    LoadContext& ctx;
    std::int64_t& records;
    std::string header;
    sequence_size_t total_bp = 0;
    bioseq::storage::sequence_stream_writer writer;

    void begin_record(std::string_view h) {
        if (h.size() > static_cast<std::size_t>(INT_MAX))
            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("bioseq.load: FASTA/FASTQ header exceeds INT_MAX")));
        header.assign(h);
        total_bp = 0;
        writer.begin(ctx.db_oid, ctx.spc_oid, ctx.main_relfilenode,
                     ctx.cols.seq_attno, ctx.parent_td, kColAlphabetCode,
                     static_cast<std::uint8_t>(ColBW), ctx.cap_bytes,
                     ctx.caches);
    }

    void chunk(bioseq::io::packed_sequence_chunk<col_unit_t> c) {
        const std::size_t bytes =
            ((static_cast<std::size_t>(c.bp_count) + col_lpu - 1) / col_lpu)
            * ColBW;
        const auto* p = reinterpret_cast<const std::uint8_t*>(c.words.data());
        writer.append_packed_bytes(std::span<const std::uint8_t>{p, bytes},
                                   c.bp_count);
        total_bp += c.bp_count;
    }

    void end_record() {
        const std::uint16_t last_pad = (total_bp == 0) ? 0
            : static_cast<std::uint16_t>(
                  ((total_bp + col_lpu - 1) / col_lpu) * col_lpu - total_bp);
        const auto locator = writer.finish(total_bp, last_pad);
        write_parent_row(ctx, header, kColAlphabetCode,
                         static_cast<std::uint8_t>(ColBW), last_pad,
                         total_bp, locator);
        ++records;
    }
};

template <typename Unit>
struct queued_load_event {
    enum class kind_t : std::uint8_t {
        begin_record,
        chunk,
        end_record,
    };

    kind_t kind = kind_t::chunk;
    std::string header{};
    std::vector<Unit> words{};
    std::uint64_t bp_count = 0;
    bool end_of_record = false;
};

template <typename Unit>
class load_event_queue {
public:
    explicit load_event_queue(std::size_t capacity) : capacity_(capacity) {}

    bool push(queued_load_event<Unit>&& event) {
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

    bool pop(queued_load_event<Unit>& event) {
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
    std::deque<queued_load_event<Unit>> queue_;
    bool closed_ = false;
};

struct load_queue_closed {};

template <sequence_alphabet A, std::size_t ColBW>
struct queued_load_stream_sink {
    using col_cfg = sequence_custom_alphabet<A,
                       typename boost::uint_t<ColBW * 8>::least>;
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
        event.bp_count = c.bp_count;
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

template <sequence_alphabet A, std::size_t ColBW,
          bioseq::io::sequence_io_file_type FT>
[[nodiscard]] std::int64_t load_records_overlapped(
        LoadContext& ctx,
        const char* file_path) {
    using col_cfg = sequence_custom_alphabet<A,
                       typename boost::uint_t<ColBW * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;

    load_event_queue<col_unit_t> queue{/*capacity=*/16};
    std::string io_error;
    std::string result_error;
    std::string producer_exception;
    std::optional<std::size_t> producer_records;

    auto& pool = bioseq::parallelism::GlobalPool::instance();
    auto group = pool.make_task_group();
    group.run(bioseq::parallelism::task_body{[
            &queue, &io_error, &result_error, &producer_exception,
            &producer_records, file_path] {
        try {
            queued_load_stream_sink<A, ColBW> sink{queue};
            auto on_fail = [&io_error](std::string&& msg) {
                if (io_error.empty()) {
                    io_error = std::move(msg);
                }
            };
            bioseq::io::reader_options opts = bioseq::io::default_reader_options;
            opts.concurrency.worker_count = 0;
            opts.concurrency.parser_count = 1;
            auto result = bioseq::io::stream_sequences_file<A, col_unit_t, FT>(
                file_path, sink, on_fail, opts);
            if (result) {
                producer_records = *result;
            } else {
                result_error = result.error();
            }
        } catch (const load_queue_closed&) {
            // The consumer hit a PostgreSQL ERROR and closed the queue. Leave
            // producer_exception empty so PG_RE_THROW preserves the original error.
        } catch (const std::exception& ex) {
            producer_exception = ex.what();
        } catch (...) {
            producer_exception = "unknown C++ exception";
        }
        queue.close();
    }});

    std::int64_t records = 0;
    load_stream_sink<A, ColBW> sink{ctx, records};
    queued_load_event<col_unit_t> event;
    PG_TRY();
    {
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
    }
    PG_CATCH();
    {
        queue.close();
        group.wait();
        PG_RE_THROW();
    }
    PG_END_TRY();
    group.wait();

    if (!producer_exception.empty()) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("bioseq.load: producer failed: %s",
                   producer_exception.c_str())));
    }
    if (!io_error.empty()) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("bioseq.load: %s", io_error.c_str())));
    }
    if (!result_error.empty()) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("bioseq.load: %s", result_error.c_str())));
    }
    if (producer_records.has_value()
        && *producer_records != static_cast<std::size_t>(records)) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("bioseq.load: producer/consumer record count mismatch "
                   "(producer=%zu, consumer=%lld)",
                   *producer_records, static_cast<long long>(records))));
    }

    JustLog("[bioseq.load] overlapped parser/writer path records={}",
            static_cast<long long>(records));
    return records;
}

template <sequence_alphabet A, std::size_t ColBW,
          bioseq::io::sequence_io_file_type FT>
[[nodiscard]] std::int64_t load_records(LoadContext& ctx, const char* file_path) {
    using col_cfg    = sequence_custom_alphabet<A,
                           typename boost::uint_t<ColBW * 8>::least>;
    using col_unit_t = typename col_cfg::storage_unit;

    constexpr std::size_t kFragmentUnits =
        bioseq::storage::kSequenceFragmentStorageUnits;
    static_assert(kFragmentUnits * 8 <= 8192,
                  "fragment payload must stay <= 8 KiB");

    if (bioseq_load_overlap_enabled) {
        bioseq::parallelism::pg::backend_sync();
        auto& pool = bioseq::parallelism::GlobalPool::instance();
        const bool may_overlap =
            bioseq::parallelism::dispatch_parallel(pool.mode())
            && pool.max_concurrency() > 1;
        if (may_overlap) {
            return load_records_overlapped<A, ColBW, FT>(ctx, file_path);
        }
    }

    std::int64_t records = 0;
    std::string io_error;
    load_stream_sink<A, ColBW> sink{ctx, records};
    auto on_fail = [&io_error](std::string&& msg) {
        if (io_error.empty()) io_error = std::move(msg);
    };

    bioseq::io::reader_options opts = bioseq::io::default_reader_options;
    opts.concurrency.worker_count = 0;
    opts.concurrency.parser_count = 1;
    auto result = bioseq::io::stream_sequences_file<A, col_unit_t, FT>(
        file_path, sink, on_fail, opts);

    if (!io_error.empty())
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("bioseq.load: %s", io_error.c_str())));
    if (!result)
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("bioseq.load: %s", result.error().c_str())));
    return records;
}

// --------------------------------------------------------------------------
// §4.2 — runtime → compile-time fan-out (alphabet × width × file_type)
// --------------------------------------------------------------------------

namespace load_dispatch {
namespace tpl = bioseq::common::templating;

template <class A, std::size_t ColBW>
struct file_format_adapter {
    template <bioseq::io::sequence_io_file_type FT>
    [[gnu::always_inline]] std::int64_t operator()(
            std::integral_constant<bioseq::io::sequence_io_file_type, FT>,
            LoadContext& ctx, const char* file_path) const {
        return load_records<A, ColBW, FT>(ctx, file_path);
    }
};

template <class A, std::size_t ColBW>
struct format_dispatch {
    LoadContext& ctx;
    const char* file_path;
    bioseq::io::sequence_io_file_type format;
    std::int64_t& out;

    void run() const {
        file_format_adapter<A, ColBW> adapter{};
        out = tpl::enum_dispatch_pack<
            std::int64_t,
            bioseq::io::sequence_io_file_type,
            bioseq::io::sequence_io_file_type::FASTA>(
                tpl::value_sequence<
                    bioseq::io::sequence_io_file_type,
                    bioseq::io::sequence_io_file_type::FASTA,
                    bioseq::io::sequence_io_file_type::FASTQ>{},
                format, adapter, ctx, file_path);
    }
};

template <class A>
struct width_dispatch {
    LoadContext& ctx;
    const char* file_path;
    bioseq::io::sequence_io_file_type format;
    std::int64_t& out;

    template <std::size_t ColBW>
    void operator()() const {
        format_dispatch<A, ColBW>{ctx, file_path, format, out}.run();
    }
};

struct alphabet_dispatch {
    LoadContext& ctx;
    const char* file_path;
    bioseq::io::sequence_io_file_type format;
    std::int64_t& out;

    template <class A>
    void operator()() const {
        dispatch_storage_unit_byte_width(ctx.cols.byte_width,
            width_dispatch<A>{ctx, file_path, format, out});
    }
};
}  // namespace load_dispatch

// The single fan-out. `ctx.cols` from resolve_load_columns; `kind.format`
// from §3. Returns records loaded.
[[nodiscard]] std::int64_t dispatch_and_stream(
        LoadContext& ctx, const char* file_path,
        bioseq::io::sequence_file_kind kind) {
    std::int64_t records_loaded = 0;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(ctx.cols.alphabet_code,
        load_dispatch::alphabet_dispatch{
            ctx, file_path, kind.format, records_loaded});
    return records_loaded;
}

// --------------------------------------------------------------------------
// §4.5 — batched index build after the streamed insert
// --------------------------------------------------------------------------

void build_indexes_if_enabled(Oid main_relid) {
    if (!bioseq_update_index_on_insert) return;     // the GUC snapshot (columnar.cppm)
    // DOC0286 — bioseq.load is one UDF inside ONE SQL command. The batched build
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

void reset_sequence_caches_after_load_storage_alloc() {
    /*
     * DOC0286 validation: streamed load can allocate new columnar storage for
     * sequence sub-table shards through the internal storageid_seq. PostgreSQL
     * keeps nextval state in a backend-local sequence cache; leaving that cache
     * warm after the internal shard allocation made the next same-backend
     * BioseqScan over the loaded relation crash. `DISCARD SEQUENCES` is the
     * minimal SQL operation that clears the state, so call the same backend
     * helper here before any same-UDF index scan and again before returning.
     */
    ResetSequenceCaches();
}

// --------------------------------------------------------------------------
// §4.4 — orchestration (open, stream, flush, close, index)
// --------------------------------------------------------------------------

void flush_pending_load_writes(LoadContext& ctx) {
    // Parent rows go through ctx.write_state; fragment rows through per-subtable
    // write states opened from ctx.caches. update_index_impl uses raw columnar
    // readers + random reads, so both layers must be flushed before the same UDF
    // starts indexing.
    FlushWriteStateForRelfilenode(
        ctx.main_relfilenode, GetCurrentSubTransactionId());
    bioseq::storage::flush_seq_cache_write_states(
        ctx.caches, GetCurrentSubTransactionId());
    CommandCounterIncrement();
}

void finish_load_write_states(LoadContext& ctx) {
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

    ResourceOwner saved = CurrentResourceOwner;
    PG_TRY();
    {
        CurrentResourceOwner = TopTransactionResourceOwner;
        for (Oid relfilenode : shard_relfilenodes) {
            ColumnarPopWriteStateForRelfilenode(
                relfilenode, GetCurrentSubTransactionId(), 0, /*commit=*/true);
        }
        ColumnarPopWriteStateForRelfilenode(
            ctx.main_relfilenode, GetCurrentSubTransactionId(), 0, /*commit=*/true);
        CurrentResourceOwner = saved;
    }
    PG_CATCH();
    {
        CurrentResourceOwner = saved;
        PG_RE_THROW();
    }
    PG_END_TRY();

    ctx.write_state = nullptr;
    ctx.caches = nullptr;
    CommandCounterIncrement();
}

// --------------------------------------------------------------------------
// §7 / §9 — typed multi-column mapping setup (runs ONCE per load)
// --------------------------------------------------------------------------

struct column_attr { std::int16_t attno; Oid atttypid; std::int32_t atttypmod; };

/// §9.2 — decode the bioseq.column_mapping[] argument. The element type OID comes
/// from the array header (ARR_ELEMTYPE) — no catalog name lookup (C2). Returns
/// user_column_mapping{column_name, data_source} views into palloc'd cstrings
/// that live in the caller's memory context for the whole load (§4.3).
[[nodiscard]] std::vector<idp::user_column_mapping>
parse_column_mapping_array(ArrayType* arr) {
    std::vector<idp::user_column_mapping> maps;
    const Oid mapping_type_oid = ARR_ELEMTYPE(arr);
    Datum* elems = nullptr; bool* nulls = nullptr; int nelems = 0;
    deconstruct_array(arr, mapping_type_oid, -1, false, 'd', &elems, &nulls, &nelems);
    TupleDesc cmdesc = lookup_rowtype_tupdesc(mapping_type_oid, -1);
    for (int i = 0; i < nelems; ++i) {
        if (nulls[i])
            ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                errmsg("bioseq.load: columns[] element %d is NULL", i + 1)));
        HeapTupleHeader th = DatumGetHeapTupleHeader(elems[i]);
        HeapTupleData tmp{};
        tmp.t_len  = HeapTupleHeaderGetDatumLength(th);
        tmp.t_data = th;
        Datum vals[2]; bool isn[2];
        heap_deform_tuple(&tmp, cmdesc, vals, isn);
        if (isn[0])
            ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                errmsg("bioseq.load: columns[%d].column_name is NULL", i + 1)));
        const char* col = text_to_cstring(DatumGetTextPP(vals[0]));
        const char* ds  = isn[1] ? "" : text_to_cstring(DatumGetTextPP(vals[1]));
        maps.push_back(idp::user_column_mapping{
            std::string_view{col}, std::string_view{ds}});
    }
    ReleaseTupleDesc(cmdesc);
    return maps;
}

/// §7 — resolve the column mapping, build the coercion table, default ExprStates,
/// and the EState/ResultRelInfo for per-row constraint enforcement. Fills the
/// parser-related fields of `ctx`. Runs once, before streaming.
void setup_typed_column_mapping(
        LoadContext& ctx, const char* path, bioseq::io::sequence_file_kind kind,
        idp::header_format requested_fmt,
        std::span<const idp::user_column_mapping> user_maps) {
    ctx.requested_fmt = requested_fmt;
    TupleDesc td = ctx.parent_td;
    const std::int16_t seq_attno = ctx.cols.seq_attno;

    // §7 step 4 / D8 — single-column PRIMARY KEY attno (composite PK -> none).
    std::int16_t pk_attno = 0;
    {
        const Oid pk_oid = find_primary_key_oid(ctx.parent_rel);
        if (OidIsValid(pk_oid)) {
            Relation pk_idx = index_open(pk_oid, AccessShareLock);
            if (pk_idx->rd_index != nullptr && pk_idx->rd_index->indnatts == 1)
                pk_attno = static_cast<std::int16_t>(
                    pk_idx->rd_index->indkey.values[0]);
            index_close(pk_idx, AccessShareLock);
        }
    }

    // §7 step 4 — flatten every non-seq column into a descriptor (NO Postgres
    // types leak to the resolver). Reject identity/generated up front (MEDIUM-02).
    std::vector<idp::column_descriptor> columns;
    std::vector<column_attr> col_attrs;
    for (int i = 0; i < td->natts; ++i) {
        Form_pg_attribute attr = TupleDescAttr(td, i);
        if (attr->attisdropped || attr->attnum <= 0) continue;
        const std::int16_t attno = static_cast<std::int16_t>(i + 1);
        if (attno == seq_attno) continue;
        if (attr->attidentity != '\0' || attr->attgenerated != '\0')
            ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("bioseq.load: column \"%s\" is an identity or generated "
                       "column; bioseq.load cannot populate identity/generated "
                       "columns", NameStr(attr->attname))));
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

    // §7 step 6 — cheap legacy passthrough: AUTO, no user maps, exactly one
    // non-seq column and it is text -> whole header verbatim, no parsing (D7/D10).
    if (requested_fmt == idp::header_format::AUTO && user_maps.empty() &&
        columns.size() == 1 && columns[0].is_text) {
        ctx.parser_active = false;
        ctx.effective_fmt = idp::header_format::PLAIN;
        ctx.cols.header_attno = col_attrs[0].attno;
        return;
    }
    ctx.parser_active = true;

    // §7 step 7 — effective format (sample + detect only in AUTO mode).
    idp::header_format effective_fmt = requested_fmt;
    if (requested_fmt == idp::header_format::AUTO) {
        auto samples = bioseq::io::sample_sequence_headers(
            path, kind.format, bioseq::io::default_reader_options);
        std::vector<std::string_view> views;
        if (samples) for (const std::string& s : *samples) views.emplace_back(s);
        effective_fmt = idp::detect_header_format(views);
        if (effective_fmt == idp::header_format::PLAIN) {
            const bool has_metadata_target =
                columns.size() > 1 ||
                (columns.size() == 1 && !columns[0].is_text) ||
                !user_maps.empty();
            if (has_metadata_target) effective_fmt = idp::header_format::AUTO;
        }
    }
    ctx.effective_fmt = effective_fmt;

    // §7 step 8 — resolve (NO_POSTGRES).
    auto map = idp::resolve_identifier_mapping(
        requested_fmt, effective_fmt, columns, user_maps);
    if (!map)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
            errmsg("bioseq.load: %s", map.error().c_str())));
    ctx.routing = std::move(map->routing);
    const std::vector<std::uint16_t> slot_to_col = std::move(map->slot_to_col);

    // §7 step 10 — NOT-NULL / default check (D9). Columns referenced by a slot may
    // still be NULL per-row (caught by ExecConstraints, E18); the up-front error
    // is only for an unmapped NOT NULL column with no default.
    std::vector<char> referenced(columns.size(), 0);
    for (std::uint16_t ci : slot_to_col) referenced[ci] = 1;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (referenced[i]) continue;
        if (columns[i].not_null && !columns[i].has_default)
            ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("bioseq.load: column \"%.*s\" is NOT NULL but is unmapped "
                       "and has no default",
                       static_cast<int>(columns[i].name.size()),
                       columns[i].name.data())));
    }

    // §7 step 11 — coercion table dest[slot].
    ctx.dest.clear();
    ctx.dest.reserve(slot_to_col.size());
    for (std::uint16_t ci : slot_to_col) {
        const column_attr& ca = col_attrs[ci];
        Oid input_fn = InvalidOid, io_param = InvalidOid;
        getTypeInputInfo(ca.atttypid, &input_fn, &io_param);
        const char typtype = get_typtype(getBaseType(ca.atttypid));  // unwrap domains
        dest_kind kind;
        if (columns[ci].is_text)
            kind = (ca.atttypmod < 0) ? dest_kind::TEXT : dest_kind::TEXT_TYPMOD;
        else if (typtype == TYPTYPE_RANGE)
            kind = dest_kind::RANGE;
        else if (typtype == TYPTYPE_MULTIRANGE)
            kind = dest_kind::MULTIRANGE;
        else
            kind = dest_kind::SCALAR;
        ctx.dest.push_back(dest_coercion{
            .attno = ca.attno,
            .column_name = std::string{columns[ci].name},
            .kind = kind,
            .input_fn = input_fn,
            .io_param = io_param,
            .typmod = ca.atttypmod,
        });
    }

    // §9.3 — EState/econtext/ResultRelInfo for default eval + ExecConstraints.
    ctx.estate = create_estate_for_relation(ctx.parent_rel);
    ctx.econtext = GetPerTupleExprContext(ctx.estate);
    ctx.result_rel_info = makeNode(ResultRelInfo);
    InitResultRelInfo(ctx.result_rel_info, ctx.parent_rel, 1, nullptr, 0);
    // NB: NO ExecOpenIndices here — this path only restores NOT NULL / CHECK
    // enforcement around ColumnarWriteRow; index maintenance stays on the
    // existing bioseq load/index policy path (§9.3).

    // §9.3 — default ExprStates for every non-seq column that has a default. The
    // executor expression path (not the const-only ColumnDefaultValue) is used so
    // volatile defaults (now(), nextval) evaluate per row (E3; REJECTED-03).
    ctx.defaults.clear();
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (!columns[i].has_default) continue;
        Node* raw = build_column_default(ctx.parent_rel, col_attrs[i].attno);
        if (raw == nullptr) continue;
        Expr* planned = expression_planner(reinterpret_cast<Expr*>(raw));
        ctx.defaults.push_back(default_col{
            col_attrs[i].attno, ExecInitExpr(planned, nullptr)});
    }
}

/// §9.3 — tear down the per-load executor state (success + PG_CATCH). Idempotent.
void teardown_load_executor(LoadContext& ctx) {
    if (ctx.estate == nullptr) return;
    AfterTriggerEndQuery(ctx.estate);
    ExecCloseResultRelations(ctx.estate);
    ExecCloseRangeTableRelations(ctx.estate);
    ExecResetTupleTable(ctx.estate->es_tupleTable, false);
    FreeExecutorState(ctx.estate);
    ctx.estate = nullptr;
    ctx.econtext = nullptr;
    ctx.result_rel_info = nullptr;
}

void cleanup_load_context(LoadContext& ctx, bool commit) {
    // §9.3 — release the per-load executor state (defaults/constraints) before
    // closing the parent relation. Idempotent (no-op if already torn down).
    teardown_load_executor(ctx);
    if (ctx.slot != nullptr) {
        ExecDropSingleTupleTableSlot(ctx.slot);
        ctx.slot = nullptr;
    }
    // DOC0286 — write-state teardown is explicit in finish_load_write_states().
    // cleanup_load_context only releases the slot and parent relation; it must not
    // touch ctx.caches because that pointer is owned by the parent write state and
    // may already have been destroyed by the per-relfilenode pop helper.
    (void)commit;
    ctx.caches = nullptr;
    ResourceOwner saved = CurrentResourceOwner;
    CurrentResourceOwner = TopTransactionResourceOwner;
    if (ctx.parent_rel != nullptr) {
        table_close(ctx.parent_rel, NoLock);
        ctx.parent_rel = nullptr;
    }
    CurrentResourceOwner = saved;
}

[[nodiscard]] std::int64_t run_load(
        Oid relid, const char* path, bioseq::io::sequence_file_kind kind,
        idp::header_format requested_fmt,
        std::span<const idp::user_column_mapping> user_maps) {
    LoadContext ctx{};
    ctx.main_relid = relid;
    std::int64_t n = 0;

    PG_TRY();
    {
        // Open the parent under TopTransactionResourceOwner, then restore the
        // caller's (executor/Portal) owner for the stream itself so the slot +
        // tuple descriptors stay owned by the Portal. The fragment SHARD relations
        // are (re-)pinned under TopTransactionResourceOwner inside
        // sequence_stream_writer::open_shard so they are released by the SAME owner
        // that cleanup_load_context/close_all use — otherwise the shards register
        // with the per-query Portal owner but close under the top-transaction
        // owner, corrupting the owner's relation array and SIGSEGV-ing after a few
        // bioseq.load calls in one backend (DOC0286 validation).
        ResourceOwner saved = CurrentResourceOwner;
        CurrentResourceOwner = TopTransactionResourceOwner;
        ctx.parent_rel = table_open(relid, RowExclusiveLock);
        CurrentResourceOwner = saved;

        ctx.parent_td        = RelationGetDescr(ctx.parent_rel);
        ctx.cols             = resolve_load_columns(ctx.parent_rel);      // §7 step 3
        // §7 — resolve the typed-column mapping + build the executor state BEFORE
        // the columnar write state is initialised, so a schema/mapping error
        // aborts cleanly (no write state to unwind) — exactly like the legacy
        // column-rule checks that used to live in resolve_load_columns.
        setup_typed_column_mapping(ctx, path, kind, requested_fmt, user_maps);
        ctx.db_oid           = ctx.parent_rel->rd_locator.dbOid;
        ctx.spc_oid          = ctx.parent_rel->rd_locator.spcOid;
        ctx.main_relfilenode = ctx.parent_rel->rd_locator.relNumber;
        ctx.cap_bytes        = bioseq::storage::read_max_sub_table_cap_bytes();

        ctx.write_state = bioseq_init_write_state(
            ctx.parent_rel, ctx.parent_td, relid, GetCurrentSubTransactionId());
        if (ctx.write_state->seq_caches == nullptr) {
            auto* c = bioseq::storage::alloc_seq_caches(
                ctx.write_state->stripeWriteContext);
            c->side                = bioseq::storage::seq_caches::Side::Write;
            c->lock_mode           = RowExclusiveLock;
            c->main_database_oid   = ctx.db_oid;
            c->main_tablespace_oid = ctx.spc_oid;
            c->main_relfilenode    = ctx.main_relfilenode;
            c->cap_bytes_snapshot  = ctx.cap_bytes;
            ctx.write_state->seq_caches = c;
        }
        // ColumnarWriteState::seq_caches is void* -> explicit cast.
        ctx.caches = static_cast<bioseq::storage::seq_caches*>(
            ctx.write_state->seq_caches);
        ctx.slot = MakeTupleTableSlot(ctx.parent_td, &TTSOpsVirtual);

        // §7 step 13 — the ONE concrete writer lives on run_load's stack; the
        // LoadContext holds only a borrowed pointer + erased ref (HIGH-01). Built
        // here (after the write state exists) so set_field's per-tuple palloc and
        // make_cstr land in ColumnarWritePerTupleContext.
        pg_identifier_writer writer{ctx, ctx.dest, ctx.defaults};
        ctx.writer = &writer;
        ctx.writer_ref = idp::make_identifier_writer_ref(writer);

        n = dispatch_and_stream(ctx, path, kind);                         // §4.2
        teardown_load_executor(ctx);   // constraints done; free estate before index build
        flush_pending_load_writes(ctx);
        // Finalize the load's write states FIRST so the streamed rows are
        // committed to columnar stripes, then build the index through SPI
        // (executor frame) reading those committed stripes — exactly what a
        // separate `bioseq.update_index` statement does (residue-free). Build
        // BEFORE ResetSequenceCaches so the portal's active snapshot is still on
        // the stack for the in-UDF SPI/columnar reads.
        finish_load_write_states(ctx);
        build_indexes_if_enabled(ctx.main_relid);                          // §4.5
        reset_sequence_caches_after_load_storage_alloc();
    }
    PG_CATCH();
    {
        cleanup_load_context(ctx, /*commit=*/false);
        PG_RE_THROW();
    }
    PG_END_TRY();

    cleanup_load_context(ctx, /*commit=*/true);
    return n;
}

}  // anonymous namespace

// --------------------------------------------------------------------------
// §5 — C entry point
// --------------------------------------------------------------------------

extern "C" {

CXX_PG_FUNCTION_INFO_V1(bioseq_load);
Datum bioseq_load(PG_FUNCTION_ARGS) {
    // §9.4 (F17) — STRICT was dropped (columns/header_format may be NULL), so the
    // two mandatory args are no longer guaranteed non-NULL: guard them FIRST.
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
            errmsg("bioseq.load: target_table and file_path must not be NULL")));
    const Oid   relid   = PG_GETARG_OID(0);               // regclass -> Oid
    char* const path    = text_to_cstring(PG_GETARG_TEXT_PP(1));
    const char* fmt_arg = PG_ARGISNULL(2) ? "auto"
                                          : text_to_cstring(PG_GETARG_TEXT_PP(2));

    // 1. file kind (NO_POSTGRES) — error -> ereport.
    auto kind_or = bioseq::io::infer_sequence_file_kind(path);
    if (!kind_or)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("bioseq.load: %s", kind_or.error().c_str())));
    const auto kind = *kind_or;

    // 2. header_format name -> enum (NO_POSTGRES).
    auto fmt_or = bioseq::io::idparse::header_format_from_sql_name(fmt_arg);
    if (!fmt_or)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("bioseq.load: %s", fmt_or.error().c_str())));
    const bioseq::io::idparse::header_format requested_fmt = *fmt_or;

    // 3. columns => bioseq.column_mapping[] (may be NULL).
    std::vector<bioseq::io::idparse::user_column_mapping> user_maps;
    if (!PG_ARGISNULL(3))
        user_maps = parse_column_mapping_array(PG_GETARG_ARRAYTYPE_P(3));

    // 4..14 — open, discover columns, resolve mapping, stream, build index, close.
    const std::int64_t n = run_load(relid, path, kind, requested_fmt, user_maps);
    JustLog("[bioseq] load: '{}' -> relid={} loaded {} records", path, relid, n);
    PG_RETURN_INT64(n);
}

}  // extern "C"
