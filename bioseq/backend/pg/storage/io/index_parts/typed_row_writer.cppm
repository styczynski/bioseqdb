/**
 * @file typed_row_writer.cppm
 * @brief Convert parsed header fields and write sequence locator rows to PostgreSQL.
 */
module;
// Include Highway and PostgreSQL headers before the named module declaration.
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

export module bioseq.postgres.storage.io:typed_row_writer;

import bioseq.common.logging; // Load  diagnostics
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

using bioseq::storage::sequence_size_t; // 64-bit sequence lengths.
using column_mapping_row = bioseq::storage::load_pg::composite_rows::column_mapping;
using column_mapping_codec = bioseq::postgres::storage::io::sql_row_codecs::column_mapping_codec;

/// Report a load error as a C++ exception while preserving PostgreSQL error details.
/// The message stays alive until PostgreSQL copies it, allowing C++ cleanup to run.
/// @param sqlstate PostgreSQL SQLSTATE code.
/// @param message Error message owned by this call.
/// @throws postgres_error On, every call.
[[noreturn]] void raise_load_error_as_cpp(int sqlstate, std::string message) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, &message] {
        ereport(ERROR, (errcode(sqlstate), errmsg("bioseq.load: %s", message.c_str())));
    });
    pg_unreachable();
}

// Find the destination sequence  column.

/// Find the installed `bioseq.seq` type.
/// @return Its PostgreSQL type OID.
/// @throws postgres_error If PostgreSQL cannot resolve the type name.
[[nodiscard]] Oid load_seq_type_oid() {
    return bioseq::postgres::support::invoke_pg_as_cpp([] {
        return bioseq::postgres::support::pg_datum::to_oid(
            bioseq::postgres::support::pg_datum::from_raw(DirectFunctionCall1(
                regtypein, bioseq::postgres::support::pg_datum::from_cstring("bioseq.seq").raw())));
    });
}

/** @brief Sequence and header columns, with the sequence alphabet and packing width. */
struct load_columns {
    /// Header column nmber, starting at one, or zero to discard the header.
    std::int16_t header_attno;
    /// Sequence column number, starting at one.
    std::int16_t seq_attno;
    /// Alphabet selected by the sequence column type modifier.
    std::uint8_t alphabet_code;
    /// Packed storage-unit width in bytes, read from the column type modifier.
    std::uint8_t byte_width;
};

/**
 * @brief Find the  single `bioseq.seq` column and its alphabet and packing width.
 * @param rel Open target table whose descriptor remains owned by the caller.
 * @return Sequence column, first text column for headers, and sequence encoding.
 * @throws postgres_error If the table has zero or multiple sequence columns, or no sequence type modifier
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
        const Form_pg_attribute attr = bioseq::postgres::support::tuple_descriptor_attribute(td, i);
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
    // Loading requires exactly one sequence column.
    if (seq_count == 0) {
        raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, "table has no bioseq.seq column");
    }
    if (seq_count > 1) {
        raise_load_error_as_cpp(
            ERRCODE_INVALID_TABLE_DEFINITION,
            fmt::format("table has {} bioseq.seq columns; exactly one is required", seq_count));
    }

    // Read the alphabet and packing width declared on the sequence column.
    const Form_pg_attribute seq_attr = bioseq::postgres::support::tuple_descriptor_attribute(td, seq_attno - 1);
    if (seq_attr->atttypmod < 0) {
        raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION,
                                "bioseq.seq column has no typmod (declare it as "
                                "bioseq.seq('<alphabet>', <width>))");
    }
    const auto tp = compact_sequence_stdc_type_params::decode(seq_attr->atttypmod);
    return load_columns{header_attno, seq_attno, tp.alphabet_code, tp.storage_unit_byte_width};
}

// Convert header values and write PostgreSQL rows.
namespace idp = bioseq::io::identifier_parser;

// varchar(n) and char(n) use their PostgreSQL input function to enforce the
// declared length limit.
/** @brief How to turn one header  field into its destination column value. */
enum class dest_kind : std::uint8_t {
    /// Text with no length limit, copied directly into a PostgreSQL text value.
    text,
    /// Text whose input function checks the declared length limit.
    text_with_typmod,
    /// Scalar value parsed by its PostgreSQL input function.
    scalar,
    /// Range iput that also accepts inclusive `lo..hi` notation.
    range,
    /// Multirange input that also accepts inclusive `lo..hi` notation.
    multirange,
};

/** @brief Conversion settings cached for one mapped destination column. */
struct dest_coercion {
    /// Destination, column number, starting at one.
    std::int16_t attno;
    /// Column name included in  input-conversion errors.
    std::string column_name;
    /// Conversion to use for this column.
    dest_kind kind;
    /// PostgreSQL input function resolved by `getTypeInputInfo`.
    Oid input_fn;
    /// Input-function type parameter, including the enum type OID for enums.
    Oid io_param;
    /// Column type modifier, such as the maximum text length.
    std::int32_t typmod;
};

/** @brief Prepared default expression for one destination column. */
struct default_col {
    /// Destination column number, starting at one.
    std::int16_t attno;
    /// Default expression evaluated by the PostgreSQL executor.
    ExprState* state;
};

/// Find a substring in a byte range that need not end with a NUL byte.
/// @param data Borrowed bytes to search.
/// @param len Number of bytes available.
/// @param needle NUL-terminated substring to find.
/// @return The first match, or null if the substring is empty or absent
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

/// Prepare a NUL-terminated string for a PostgreSQL type input  function.
/// Trim spaces at both ends. A range value with exactly one `..` becomes
/// inclusive `[lo,hi]`, while a multirange becomes `{[lo,hi]}`.
/// @param data Borrowed input bytes.
/// @param len Input length in bytes.
/// @param dc Destination conversion settings.
/// @param mctx Memory context for the result.
/// @return Prepared string allocated in mctx.
/// @throws PostgreSQL ERROR If allocatiog fails.
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
        // Pass other range text to PostgreSQL after trimming spaces.
    }
    char* out = bioseq::postgres::support::invoke_pg_as_cpp([mctx, len] {
        return static_cast<char*>(MemoryContextAlloc(mctx, static_cast<std::size_t>(len) + 1));
    });
    std::memcpy(out, data, static_cast<std::size_t>(len));
    out[len] = '\0';
    return out;
}

// State shared while loading one file.

/** @brief Table, parser and writer state used during one load call. */
struct load_context {
    /// Target table OID used for index maintenance after  loading.
    Oid main_relid;
    /// Database and tablespace containing the target table.
    Oid db_oid, spc_oid;
    /// Target table file number.
    RelFileNumber main_relfilenode;
    /// Column-storage ID used to detect replacement of the target table storage.
    std::uint64_t main_storage_id = 0;
    /// Open target table reference, held by the transaction resource owner.
    Relation parent_rel;
    /// Column description borrowed from parent_rel.
    TupleDesc parent_td;
    /// Sequence and header columns, with the sequence encoding.
    load_columns cols;
    /// Fragment table caches shared with the parent write state.
    bioseq::storage::seq_caches* caches;
    /// Parent column writer owned by PostgreSQL write-state management.
    column_write_state* write_state;
    /// Maximum payload bytes in one fragment table shard.
    sequence_size_t cap_bytes;
    /// Tuple slot used to assemble each parent row.
    TupleTableSlot* slot;

    // Share one header  parser across, alphabet, packing-width and file-format
    // specializations to keep the generated code smaller.
    bool parser_active = false;
    idp::header_format requested_fmt = idp::header_format::automatic;
    idp::header_format effective_fmt = idp::header_format::automatic;
    idp::identifier_routing routing;
    idp::identifier_parse_state parse_state;
    idp::identifier_writer_reference writer_ref;
    std::vector<dest_coercion> dest; // One entry per parser destination slot.
    std::vector<default_col> defaults;
    EState* estate = nullptr;
    ExprContext* econtext = nullptr;
    ResultRelInfo* result_rel_info = nullptr;

    // Details included when PostgreSQL reports an error for the current record
    std::uint64_t record_no = 0;
    std::string_view current_header;
    const char* current_column_name = nullptr;
};

/// Add the record number, column and a shortened hepder to a PostgreSQL error.
/// @param arg Active load_context.
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

/** @brief Add record details to errors raised while writing one row. */
struct load_error_context_scope {
    /// Load context holding the current record and column details.
    load_context& ctx;
    /// Callback node linked into PostgreSQL's backend-local stack.
    ErrorContextCallback callback{};

    /// Attach record details to PostgreSQL errors until this scope ends.
    /// @param context Load context that outlives this  scope.
    /// @param header Header bytes that remain valid throughout this scope.
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

/// Convert parsed header fields into PostgreSQL values and write completed rows.
struct pg_identifier_writer {
    /// Active load context that outlives this writer.
    load_context& ctx;
    /// Conversion settings borrowed for each parser destination.
    std::span<const dest_coercion> dest;
    /// Defaults applied to destination columns still set to NULL.
    std::span<const default_col> defaults;

    /// Clear the row slot and set every column to SQL NULL.
    void begin_tuple() {
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.clear_all_to_null();
        });
    }

    /// Set one mapped column to SQL NULL.
    /// @param slot Parser destination number, starting at zero.
    void set_null(idp::destination_slot slot) {
        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_null(dest[slot].attno);
    }
    /// Convert one parsed field to its PostgreSQL column type.
    /// @param slot Parser destination number, starting at zero.
    /// @param data Borrowed field bytes, which need not end with NUL.
    /// @param len Field length  in bytes.
    /// @throws postgres_error If conversion or allocation fails.
    void set_field(idp::destination_slot slot, const char* data, std::int32_t len) {
        const dest_coercion& dc = dest[slot];
        ctx.current_column_name = dc.column_name.c_str(); // Name the column if conversion fails.
        const MemoryContext per_tuple = get_column_write_per_tuple_context(ctx.write_state);
        Datum v;
        if (dc.kind == dest_kind::text) { // Text without a length limit.
            v = bioseq::postgres::support::invoke_pg_as_cpp([per_tuple, data, len] {
                const MemoryContext old = MemoryContextSwitchTo(per_tuple);
                const Datum result = PointerGetDatum(cstring_to_text_with_len(data, len));
                MemoryContextSwitchTo(old);
                return result;
            });
        } else { // Values that need PostgreSQL type conversion.
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
    /// Fill NULL columns from defaults, check constraints and write the parent row.
    /// @throws postgres_error If a efault, constraint or write fails.
    ///
    /// Keep temporary values alive until write_column_row has copied their bytes.
    void end_tuple() {
        // Apply defaults to columns still set to NULL. Keep values allocated by default
        // expressions alive until write_column_row has copied them
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
        // The row  writer has copied referenced values, so temporary memory can now be reused.
        if (ctx.econtext) {
            bioseq::postgres::support::invoke_pg_as_cpp([this] { ResetExprContext(ctx.econtext); });
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [this] { MemoryContextReset(get_column_write_per_tuple_context(ctx.write_state)); });
    }
    /// Finish the parser callback sequence. All rows have, already been written.
    void finish() {}

    /** @return The number of identifier-parser destinations. */
    [[nodiscard]] std::size_t destination_count() const noexcept {
        return dest.size();
    }
};
static_assert(idp::identifier_destination_writer<pg_identifier_writer>);

// Reuse this parent-row writer for every alphabet, packing width and input format.
/// Write the parent locator and header values before publishing its sequence source.
/// @param ctx Load context.
/// @param tuple_writer Header/default writer.
/// @param header Borrowed FASTA or FASTQ header.
/// @param alphabet_code Sequence alphabet.
/// @param byte_width Packed unit width in bytes.
/// @param last_pad Unused letters in the final  unit.
/// @param total_bp Sequence length in letters.
/// @param pending_source Unpublished source consumed after writing the parent.
/// @throws postgres_error If parsing, constraints, writing or publication fails.
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

    tuple_writer.begin_tuple(); // Begin with all columns set to NULL.
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
    tuple_writer.end_tuple(); // Fill defaults, check constraints and write the row.
    pending_source.publish_after_parent_row();
}
