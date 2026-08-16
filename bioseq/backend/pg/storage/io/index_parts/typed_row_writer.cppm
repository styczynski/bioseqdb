/**
 * @file typed_row_writer.cppm
 * @brief Discovers typed destination columns and writes validated PostgreSQL rows.
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
export module bioseq.postgres.storage.io:typed_row_writer;

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

using bioseq::storage::sequence_size_t; // bp counts (uint64 — 6 Gbp safe)
using column_mapping_row = bioseq::storage::load_pg::composite_rows::column_mapping;
using column_mapping_codec = bioseq::postgres::storage::io::sql_row_codecs::column_mapping_codec;

/// Raise one loader error through the checked PG leaf seam. The owned message
/// remains alive while PostgreSQL constructs/copies ErrorData, then ordinary
/// C++ unwinding releases every caller-owned queue/string/writer object.
/// @param sqlstate PostgreSQL SQLSTATE code for the error.
/// @param message Owning diagnostic text.
/// @throws PostgreSQL ERROR unconditionally.
[[noreturn]] void raise_load_error_as_cpp(int sqlstate, std::string message) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, &message] {
        ereport(ERROR, (errcode(sqlstate), errmsg("bioseq.load: %s", message.c_str())));
    });
    pg_unreachable();
}

// --------------------------------------------------------------------------
// Column discovery
// --------------------------------------------------------------------------

/// Resolve the installed `bioseq.seq` type.
/// @return Type OID, or `InvalidOid` when PostgreSQL cannot resolve the name.
[[nodiscard]] Oid load_seq_type_oid() {
    return bioseq::postgres::support::invoke_pg_as_cpp([] {
        return bioseq::postgres::support::pg_datum::to_oid(
            bioseq::postgres::support::pg_datum::from_raw(DirectFunctionCall1(
                regtypein, bioseq::postgres::support::pg_datum::from_cstring("bioseq.seq").raw())));
    });
}

/** @brief Resolved destination sequence column and its encoded typmod facts. */
struct load_columns {
    /// One-based header attribute, or zero when headers are discarded.
    std::int16_t header_attno;
    /// One-based attribute of the required sequence column.
    std::int16_t seq_attno;
    /// Alphabet code decoded from the sequence typmod.
    std::uint8_t alphabet_code;
    /// Packed storage-unit width in bytes decoded from the typmod.
    std::uint8_t byte_width;
};

/**
 * @brief Finds the single `bioseq.seq` column and decodes its typmod.
 * @param rel Open destination relation whose descriptor remains caller-owned.
 * @return Resolved header and sequence attributes with sequence encoding.
 * @throws postgres_error If the relation has no usable unique sequence column.
 */
[[nodiscard]] load_columns resolve_load_columns(Relation rel) {
    const TupleDesc td = RelationGetDescr(rel);
    const Oid seq_type_oid = load_seq_type_oid();
    if (!OidIsValid(seq_type_oid)) {
        raise_load_error_as_cpp(ERRCODE_UNDEFINED_OBJECT,
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
        } else if ((attr->atttypid == TEXTOID || attr->atttypid == VARCHAROID ||
                    attr->atttypid == BPCHAROID) &&
                   header_attno == 0) {
            header_attno = static_cast<std::int16_t>(i + 1); // first text column
        }
    }
    // Exactly one seq column (unchanged).
    if (seq_count == 0) {
        raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, "table has no bioseq.seq column");
    }
    if (seq_count > 1) {
        raise_load_error_as_cpp(
            ERRCODE_INVALID_TABLE_DEFINITION,
            fmt::format("table has {} bioseq.seq columns; exactly one is required", seq_count));
    }

    // (alphabet, width) off the seq column's typmod — NEVER hardcoded.
    const Form_pg_attribute seq_attr = TupleDescAttr(td, seq_attno - 1);
    if (seq_attr->atttypmod < 0) {
        raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION,
                                "bioseq.seq column has no typmod (declare it as "
                                "bioseq.seq('<alphabet>', <width>))");
    }
    const auto tp = compact_sequence_stdc_type_params::decode(seq_attr->atttypmod);
    return load_columns{header_attno, seq_attno, tp.alphabet_code, tp.storage_unit_byte_width};
}

// --------------------------------------------------------------------------
// Typed-column coercion and PostgreSQL row writing
// --------------------------------------------------------------------------
namespace idp = bioseq::io::identifier_parser;

// text_with_typmod = varchar(n)/bpchar(n) with a length constraint -> must run the
// input function so the length is enforced (NOT the raw cstring_to_text path).
/** @brief PostgreSQL coercion route selected for one destination column. */
enum class dest_kind : std::uint8_t {
    /// Unconstrained text accepted without invoking a type input function.
    text,
    /// Text whose typmod must be enforced by its input function.
    text_with_typmod,
    /// Scalar value parsed by its PostgreSQL input function.
    scalar,
    /// Range value with optional loader shorthand conversion.
    range,
    /// Multirange value with optional loader shorthand conversion.
    multirange,
};

/** @brief Cached input-function metadata for one mapped destination column. */
struct dest_coercion {
    /// One-based destination attribute number.
    std::int16_t attno;
    /// Owning column name retained for PostgreSQL error context.
    std::string column_name;
    /// Selected input conversion route.
    dest_kind kind;
    /// PostgreSQL input function resolved by `getTypeInputInfo`.
    Oid input_fn;
    /// Input-function type parameter, including the enum type OID for enums.
    Oid io_param;
    /// Destination typmod passed to the input function.
    std::int32_t typmod;
};

/** @brief Compiled default expression for one destination attribute. */
struct default_col {
    /// One-based destination attribute number.
    std::int16_t attno;
    /// Executor-owned compiled default expression.
    ExprState* state;
};

/// Two-byte substring scan without relying on the POSIX `memmem` extension.
/// @param data Borrowed byte range that need not be null-terminated.
/// @param len Length of `data` in bytes.
/// @param needle Null-terminated substring to locate.
/// @return Pointer into `data`, or null when no match exists.
[[nodiscard]] const char* find_substr(const char* data, int len, const char* needle) {
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
/// canonical inclusive `[lo,hi]` (multirange: `{[lo,hi]}`). Otherwise the trimmed
/// text is passed straight to `<type>range_in`/`<type>multirange_in`.
/// @param data Borrowed input bytes that need not be null-terminated.
/// @param len Input length in bytes.
/// @param dc Destination conversion metadata.
/// @param mctx Memory context that owns the returned string.
/// @return Null-terminated input text allocated in `mctx`.
/// @throws PostgreSQL ERROR when allocation fails.
[[nodiscard]] char*
make_cstr(const char* data, int len, const dest_coercion& dc, MemoryContext mctx) {
    while (len > 0 && data[0] == ' ') {
        ++data;
        --len;
    }
    while (len > 0 && data[len - 1] == ' ') {
        --len;
    }
    const bool is_range = (dc.kind == dest_kind::range || dc.kind == dest_kind::multirange);
    if (is_range) {
        const char* d0 = find_substr(data, len, "..");
        const bool one_dots =
            d0 && !find_substr(d0 + 2, len - static_cast<int>(d0 - data) - 2, "..");
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
            char* out = bioseq::postgres::support::invoke_pg_as_cpp([mctx, lol, hil] {
                return static_cast<char*>(
                    MemoryContextAlloc(mctx, static_cast<std::size_t>(lol + hil + 6)));
            });
            int k = 0;
            if (mr) {
                out[k++] = '{';
            }
            out[k++] = '[';
            std::memcpy(out + k, lo, static_cast<std::size_t>(lol));
            k += lol;
            out[k++] = ',';
            std::memcpy(out + k, hi, static_cast<std::size_t>(hil));
            k += hil;
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
        return static_cast<char*>(MemoryContextAlloc(mctx, static_cast<std::size_t>(len) + 1));
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
    /// Parent relation OID used by post-load index maintenance.
    Oid main_relid;
    /// Database and tablespace components of the parent identity.
    Oid db_oid, spc_oid;
    /// File component of the parent identity.
    RelFileNumber main_relfilenode;
    /// Storage-generation identity of the parent relation.
    std::uint64_t main_storage_id = 0;
    /// Open parent relation owned by the load invocation.
    Relation parent_rel;
    /// Descriptor borrowed from `parent_rel`.
    TupleDesc parent_td;
    /// Resolved sequence-column encoding and destinations.
    load_columns cols;
    /// Write-side shard cache owned by the load invocation.
    bioseq::storage::seq_caches* caches;
    /// Parent column writer owned by PostgreSQL write-state management.
    column_write_state* write_state;
    /// Per-shard capacity in bytes.
    sequence_size_t cap_bytes;
    /// Executor tuple slot owned by this invocation.
    TupleTableSlot* slot;

    // Keep the PostgreSQL writer non-templated so identifier parsing is not
    // multiplied by the alphabet, storage-width, and file-type dispatch matrix.
    bool parser_active = false;
    idp::header_format requested_fmt = idp::header_format::automatic;
    idp::header_format effective_fmt = idp::header_format::automatic;
    idp::identifier_routing routing;
    idp::identifier_parse_state parse_state;
    idp::identifier_writer_reference writer_ref;
    std::vector<dest_coercion> dest; // index == slot
    std::vector<default_col> defaults;
    EState* estate = nullptr;
    ExprContext* econtext = nullptr;
    ResultRelInfo* result_rel_info = nullptr;

    // Read only by the per-record ErrorContextCallback.
    std::uint64_t record_no = 0;
    std::string_view current_header;
    const char* current_column_name = nullptr;
};

/// Add bounded record context to coercion, default, or constraint failures.
/// @param arg Pointer to the active `load_context`.
void bioseq_load_error_context(void* arg) {
    auto* ctx = static_cast<load_context*>(arg);
    const std::string_view h = ctx->current_header;
    const int hlen = static_cast<int>(std::min<std::size_t>(h.size(), 240));
    if (ctx->current_column_name != nullptr) {
        errcontext("bioseq.load: record %llu, column \"%s\", header \"%.*s\"",
                   static_cast<unsigned long long>(ctx->record_no),
                   ctx->current_column_name,
                   hlen,
                   h.data());
    } else {
        errcontext("bioseq.load: record %llu, header \"%.*s\"",
                   static_cast<unsigned long long>(ctx->record_no),
                   hlen,
                   h.data());
    }
}

/** @brief Installs bounded record context for the duration of one row write. */
struct load_error_context_scope {
    /// Context whose diagnostic fields and callback stack are managed.
    load_context& ctx;
    /// Callback node linked into PostgreSQL's backend-local stack.
    ErrorContextCallback callback{};

    /// Install record context until this scope is destroyed.
    /// @param context Active load context that outlives the scope.
    /// @param header Borrowed header that remains valid for the scope.
    load_error_context_scope(load_context& context, std::string_view header) : ctx(context) {
        ctx.current_header = header;
        ctx.current_column_name = nullptr;
        callback.callback = bioseq_load_error_context;
        callback.arg = &ctx;
        callback.previous = error_context_stack;
        error_context_stack = &callback;
    }

    /// Restore the prior PostgreSQL error-context callback and clear borrowed data.
    ~load_error_context_scope() noexcept {
        error_context_stack = callback.previous;
        ctx.current_header = {};
        ctx.current_column_name = nullptr;
    }
};

/// PostgreSQL sink that routes each byte range through its column's cached
/// input function.
struct pg_identifier_writer {
    /// Active load context that outlives this writer.
    load_context& ctx;
    /// Slot-indexed coercion metadata borrowed for this writer's lifetime.
    std::span<const dest_coercion> dest;
    /// Default expressions for unmapped or unset destination columns.
    std::span<const default_col> defaults;

    /// Clear the destination slot to an all-NULL virtual tuple.
    void begin_tuple() {
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.clear_all_to_null();
        });
    }

    /// Set one parser destination to SQL NULL.
    /// @param slot Zero-based identifier-parser destination slot.
    void set_null(idp::destination_slot slot) {
        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_null(dest[slot].attno);
    }
    /// Convert and assign one parser destination field.
    /// @param slot Zero-based identifier-parser destination slot.
    /// @param data Borrowed field bytes that need not be null-terminated.
    /// @param len Field length in bytes.
    /// @throws PostgreSQL ERROR when input conversion or allocation fails.
    void set_field(idp::destination_slot slot, const char* data, std::int32_t len) {
        const dest_coercion& dc = dest[slot];
        ctx.current_column_name = dc.column_name.c_str(); // errcontext if input fn errors
        const MemoryContext per_tuple = get_column_write_per_tuple_context(ctx.write_state);
        Datum v;
        if (dc.kind == dest_kind::text) { // plain text/varchar/bpchar, no typmod
            v = bioseq::postgres::support::invoke_pg_as_cpp([per_tuple, data, len] {
                const MemoryContext old = MemoryContextSwitchTo(per_tuple);
                const Datum result = PointerGetDatum(cstring_to_text_with_len(data, len));
                MemoryContextSwitchTo(old);
                return result;
            });
        } else { // text_with_typmod/scalar/range/multirange
            char* cstr = make_cstr(data, len, dc, per_tuple);
            v = bioseq::postgres::support::invoke_pg_as_cpp([per_tuple, &dc, cstr] {
                const MemoryContext old = MemoryContextSwitchTo(per_tuple);
                const Datum result =
                    OidInputFunctionCall(dc.input_fn, cstr, dc.io_param, dc.typmod);
                MemoryContextSwitchTo(old);
                return result;
            });
        }
        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
            dc.attno, bioseq::postgres::support::pg_datum::from_raw(v));
        ctx.current_column_name = nullptr;
    }
    /// Apply defaults, enforce constraints, and write the completed parent row.
    /// @throws PostgreSQL ERROR when evaluation, constraints, or persistence fail.
    ///
    /// Per-tuple memory is reset only after `write_column_row` has copied every
    /// by-reference datum.
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
                [&d, this, &isnull] { return ExecEvalExpr(d.state, ctx.econtext, &isnull); });
            if (isnull) {
                bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_null(d.attno);
            } else {
                bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
                    d.attno, bioseq::postgres::support::pg_datum::from_raw(v));
            }
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.store_virtual(); });
        if (ctx.result_rel_info && ctx.parent_rel->rd_att->constr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [this] { ExecConstraints(ctx.result_rel_info, ctx.slot, ctx.estate); });
        }
        (void)write_column_row(ctx.write_state, ctx.slot->tts_values, ctx.slot->tts_isnull);
        // Reset temporary contexts only after write_column_row has copied
        // the by-reference datums.
        if (ctx.econtext) {
            bioseq::postgres::support::invoke_pg_as_cpp([this] { ResetExprContext(ctx.econtext); });
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { MemoryContextReset(get_column_write_per_tuple_context(ctx.write_state)); });
    }
    /// Complete the identifier-writer protocol. This sink has no buffered state.
    void finish() {}

    /** @return The number of identifier-parser destinations. */
    [[nodiscard]] std::size_t destination_count() const noexcept {
        return dest.size();
    }
};
static_assert(idp::identifier_destination_writer<pg_identifier_writer>);

// The non-templated parent writer keeps identifier parsing outside the
// alphabet, storage-width, and file-type dispatch matrix.
/// Write one parent locator stub and publish its source epoch afterward.
/// @param ctx Active load invocation owning relation and writer state.
/// @param tuple_writer Identifier sink used to populate non-sequence columns.
/// @param header Borrowed FASTA or FASTQ header for this record.
/// @param alphabet_code Persisted sequence alphabet code.
/// @param byte_width Packed storage-unit width in bytes.
/// @param last_pad Unused letters in the final packed unit.
/// @param total_bp Logical sequence length in letters.
/// @param pending_source Sole publication authority consumed after the parent
/// row has been written successfully.
/// @throws PostgreSQL ERROR when header routing, row constraints, persistence,
/// or publication fails.
void write_parent_row(load_context& ctx,
                      pg_identifier_writer& tuple_writer,
                      std::string_view header,
                      std::uint8_t alphabet_code,
                      std::uint8_t byte_width,
                      std::uint16_t last_pad,
                      sequence_size_t total_bp,
                      bioseq::storage::pending_sequence_source&& pending_source) {
    const bioseq::storage::sequence_locator locator = pending_source.locator();
    bioseq::pg_types::seq* stub = bioseq::postgres::support::invoke_pg_as_cpp(
        [write_state = ctx.write_state, alphabet_code, byte_width, last_pad, total_bp, locator] {
            return bioseq::storage::alloc_locator_stub_in(
                get_column_write_per_tuple_context(write_state),
                alphabet_code,
                byte_width,
                last_pad,
                total_bp,
                locator);
        });

    ++ctx.record_no;
    load_error_context_scope error_context{ctx, header};

    tuple_writer.begin_tuple(); // ExecClearTuple + all-NULL
    bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
        ctx.cols.seq_attno, bioseq::postgres::support::pg_datum::from_pointer(stub));

    if (ctx.parser_active) {
        auto routed = idp::parse_and_route_header(
            ctx.effective_fmt, header, ctx.routing, ctx.parse_state, ctx.writer_ref);
        if (!routed) {
            raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, routed.error());
        }
    } else if (ctx.cols.header_attno != 0) {
        if (header.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                    "FASTA/FASTQ header exceeds INT_MAX");
        }
        text* htext = bioseq::postgres::support::invoke_pg_as_cpp([&ctx, header] {
            const MemoryContext old =
                MemoryContextSwitchTo(get_column_write_per_tuple_context(ctx.write_state));
            text* result = cstring_to_text_with_len(header.data(), static_cast<int>(header.size()));
            MemoryContextSwitchTo(old);
            return result;
        });
        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
            ctx.cols.header_attno, bioseq::postgres::support::pg_datum::from_pointer(htext));
    }
    tuple_writer.end_tuple(); // defaults, ExecConstraints, write_column_row, ctx reset
    pending_source.publish_after_parent_row();
}
