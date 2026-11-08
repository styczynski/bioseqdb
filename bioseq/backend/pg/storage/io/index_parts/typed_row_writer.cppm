/**
 * @file typed_row_writer.cppm
 * @brief Convert parsed header fields and write sequence locator rows to PostgreSQL.
 */
module;
// Include Highway and PostgreSQL headers before the named module declaration.
// Imported sequence modules also depend on these declarations.
#include <bioseq/pg_compat.h>
#include <algorithm>
#include <map>
#include <array>
#include <boost/container/small_vector.hpp>
#include <boost/integer.hpp>
#include <condition_variable>
#include <concepts>
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
#include <utility>
#include <vector>
extern "C" {
#include "postgres.h"

#include "access/relation.h"     // index_open / index_close
#include "access/table.h"        // table_open / table_close
#include "access/xact.h"         // CommandCounterIncrement
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"   // TEXTOID, VARCHAROID, BPCHAROID
#include "commands/sequence.h"   // ResetSequenceCaches
#include "commands/trigger.h"    // Index constraint and AFTER INSERT trigger events.
#include "executor/executor.h"   // ExecConstraints, ExecInitExpr, ExecEvalExpr
#include "executor/tuptable.h"   // TupleTableSlot, MakeTupleTableSlot, TTSOpsVirtual, Exec*Tuple
#include "mb/pg_wchar.h"         // Imported bytes must match the database encoding.
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
import bioseq.postgres.storage.io.scalar_codec;
import bioseq.postgres.configuration.core;
import bioseq.storage.size; // sequence_size_t
import bioseq.storage.layout;
import bioseq.common.templating.runtime_dispatch; // dispatch_exact_value_or_default
import bioseq.common.templating.value_sequence;
import bioseq.sequences;                       // Sequence alphabets and packed storage types.

import bioseq.postgres.types.compact_sequence; // PostgreSQL sequence values and supported alphabets.
import bioseq.postgres.storage.scanner;
import bioseq.postgres.storage.manager; // Sequence locators, fragment writers and relation caches.


import bioseq.postgres.table.io; // Column table row writes and per-row memory contexts.
import bioseq.postgres.column.table_access; // Logical row number to PostgreSQL TID.
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

// Backend-local, one-shot test state. Workers never access PostgreSQL test controls.
#if defined(BIOSEQ_TEST_INDEXES)
int load_failure_stage_for_test = 0;
#endif

void arm_load_failure_for_test(int stage) {
#if defined(BIOSEQ_TEST_INDEXES)
    if (stage < 0 || stage > 7)
        raise_load_error_as_cpp(ERRCODE_INVALID_PARAMETER_VALUE, "load failure stage must be between zero and seven");
    load_failure_stage_for_test = stage;
#else
    (void)stage;
    raise_load_error_as_cpp(ERRCODE_FEATURE_NOT_SUPPORTED, "load failure injection requires a test build");
#endif
}

/** Exercise rollback at ownership transitions without PostgreSQL calls on parser workers. */
void inject_load_failure(int stage) {
#if defined(BIOSEQ_TEST_INDEXES)
    if (load_failure_stage_for_test == stage) {
        load_failure_stage_for_test = 0;
        raise_load_error_as_cpp(ERRCODE_INTERNAL_ERROR, fmt::format("injected load failure at stage {}", stage));
    }
#else
    (void)stage;
#endif
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

/** Classify header destinations by their base type while retaining declared IO.
 * Domains and PostgreSQL name values participate in the same header inference
 * as text; their original input functions still enforce constraints and layout. */
[[nodiscard]] bool load_type_accepts_header(Oid type) {
    const auto base = bioseq::postgres::support::invoke_pg_as_cpp([type] { return getBaseType(type); });
    return base == TEXTOID || base == VARCHAROID || base == BPCHAROID || base == NAMEOID;
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
    std::int16_t alignment_attno{};
    std::int16_t quality_attno{};
    std::uint8_t quality_alphabet_code{};
    std::uint8_t quality_byte_width{};
};

/**
 * @brief Find the  single `bioseq.seq` column and its alphabet and packing width.
 * @param rel Open target table whose descriptor remains owned by the caller.
 * @return Sequence column, first text column for headers, and sequence encoding.
 * @throws postgres_error If the table has zero or multiple sequence columns, or no sequence type modifier
 */
[[nodiscard]] load_columns resolve_load_columns(Relation rel, std::string_view quality_name = {},
        bool fastq = false, std::string_view alignment_name = {}) {
    const TupleDesc td = RelationGetDescr(rel);
    const Oid seq_type_oid = load_seq_type_oid();
    if (!OidIsValid(seq_type_oid)) {
        raise_load_error_as_cpp(ERRCODE_UNDEFINED_OBJECT,
                                "bioseq.seq type not found (is the extension installed?)");
    }

    std::int16_t header_attno = 0;
    std::int16_t seq_attno = 0;
    int seq_count = 0;
    load_columns result{};
    for (int i = 0; i < td->natts; ++i) {
        const Form_pg_attribute attr = bioseq::postgres::support::tuple_descriptor_attribute(td, i);
        if (attr->attisdropped || attr->attnum <= 0) {
            continue;
        }
        if (!alignment_name.empty() && alignment_name == NameStr(attr->attname)) {
            result.alignment_attno = static_cast<std::int16_t>(i + 1);
            continue;
        }
        if (attr->atttypid == seq_type_oid) {
            if (attr->atttypmod >= 0 && pgseq_is_cigar_alphabet(
                    compact_sequence_stdc_type_params::decode(attr->atttypmod).alphabet_code)) continue;
            if (attr->atttypmod >= 0 && is_quality_alphabet_code(
                    compact_sequence_stdc_type_params::decode(attr->atttypmod).alphabet_code)) {
                if (fastq && quality_name == NameStr(attr->attname)) {
                    const auto type = compact_sequence_stdc_type_params::decode(attr->atttypmod);
                    result.quality_attno = i + 1;
                    result.quality_alphabet_code = type.alphabet_code;
                    result.quality_byte_width = type.storage_unit_byte_width;
                }
                continue;
            }
            ++seq_count;
            seq_attno = static_cast<std::int16_t>(i + 1);
        } else if (load_type_accepts_header(attr->atttypid) && header_attno == 0) {
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
    if (tp.alphabet_code >= pgseq_supported_biological_alphabets::alphabet_count
        || !pgseq_supported_storage_unit_byte_widths::contains_runtime(tp.storage_unit_byte_width))
        raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, "sequence destination requires an admitted biological alphabet and width");
    if (seq_attr->attidentity || seq_attr->attgenerated)
        raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, "sequence destination must be writable");
    if (fastq && !result.quality_attno)
        raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, "FASTQ quality destination is missing");
    result.header_attno = header_attno;
    result.seq_attno = seq_attno;
    result.alphabet_code = tp.alphabet_code;
    result.byte_width = tp.storage_unit_byte_width;
    return result;
}

// Convert header values and write PostgreSQL rows.
namespace idp = bioseq::io::identifier_parser;

// varchar(n) and char(n) use their PostgreSQL input function to enforce the
// declared length limit.
/** @brief How to turn one header  field into its destination column value. */
enum class dest_kind : std::uint8_t {
    /// Text with no length limit, copied directly into a PostgreSQL text value.
    text,
    /// Text-like values requiring declared input for typmods, domains or name layout.
    text_input,
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

/** Domain NULL checks cached per column for the lifetime of the load executor.
 * Non-NULL inputs and evaluated defaults already pass through declared type
 * coercion. This state covers explicit NULLs and absent values without defaults. */
struct load_domain_column {
    AttrNumber attribute;
    Oid type;
    bool has_default;
    void* constraint_cache = nullptr;
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
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::array<std::uint32_t, 256>> rank_mappings;
    std::vector<bool> assigned_columns; // Explicit NULL values must not invoke column defaults.
    std::vector<default_col> defaults;
    std::vector<load_domain_column> domain_columns;
    EState* estate = nullptr;
    ExprContext* econtext = nullptr;
    ResultRelInfo* result_rel_info = nullptr;
    bool indexes_read_sequences = false;

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

/** Run executor checks over bounded staged sources without publishing them.
 * Temporary views are released before their provider and snapshot; DEFAULT
 * values keep their original context. Scalar-only indexes avoid source reads. */
template<std::invocable Action>
void with_load_pending_sequences(load_context& ctx,
        const bioseq::storage::pending_sequence_source* primary,
        const bioseq::storage::pending_sequence_source* quality,
        bool needs_sequences, Action action) {
    if (!needs_sequences || (primary == nullptr && quality == nullptr)) {
        action();
        return;
    }
    Snapshot snapshot = nullptr;
    bioseq::storage::pg_sequence_source_provider* provider = nullptr;
    ExprContext* check_context = nullptr;
    auto* const saved_context = ctx.estate->es_per_tuple_exprcontext;
    const auto saved_memory = CurrentMemoryContext;
    const std::array<AttrNumber, 2> attributes{primary ? ctx.cols.seq_attno : AttrNumber{0},
        quality ? ctx.cols.quality_attno : AttrNumber{0}};
    const std::array<const bioseq::storage::pending_sequence_source*, 2> sources{primary, quality};
    std::array<Datum, 2> locators{};
    for (std::size_t i = 0; i < attributes.size(); ++i)
        if (attributes[i] > 0) locators[i] = ctx.slot->tts_values[attributes[i] - 1];
    std::exception_ptr failure;
    try {
        bioseq::postgres::support::invoke_pg_as_cpp([&ctx, &snapshot, &check_context] {
            bioseq::storage::flush_seq_cache_write_states(ctx.caches, GetCurrentSubTransactionId());
            CommandCounterIncrement();
            snapshot = RegisterSnapshot(GetTransactionSnapshot());
            check_context = CreateExprContext(ctx.estate);
            ctx.estate->es_per_tuple_exprcontext = check_context;
        });
        provider = bioseq::storage::create_scan_pg_sequence_source_provider(ctx.parent_rel, snapshot);
        for (std::size_t i = 0; i < attributes.size(); ++i) {
            if (attributes[i] <= 0) continue;
            ctx.slot->tts_values[attributes[i] - 1] = bioseq::storage::promote_pending_pg_sequence_source(
                provider, attributes[i], locators[i], *sources[i], check_context->ecxt_per_tuple_memory);
        }
        action();
    } catch (...) { failure = std::current_exception(); }
    ctx.estate->es_per_tuple_exprcontext = saved_context;
    MemoryContextSwitchTo(saved_memory);
    for (std::size_t i = 0; i < attributes.size(); ++i)
        if (attributes[i] > 0) ctx.slot->tts_values[attributes[i] - 1] = locators[i];
    const auto cleanup = [&failure](auto&& action) {
        try { action(); } catch (...) { if (!failure) failure = std::current_exception(); }
    };
    if (check_context) cleanup([check_context, &failure] {
        bioseq::postgres::support::invoke_pg_as_cpp([check_context, succeeded = !failure] { FreeExprContext(check_context, succeeded); });
    });
    if (provider) cleanup([provider, &failure] {
        bioseq::postgres::support::invoke_pg_as_cpp([provider, succeeded = !failure] {
            bioseq::storage::close_pg_sequence_source_provider(provider, succeeded);
        });
    });
    if (snapshot) cleanup([snapshot] {
        bioseq::postgres::support::invoke_pg_as_cpp([snapshot] { UnregisterSnapshot(snapshot); });
    });
    if (failure) std::rethrow_exception(failure);
}

/** Check NULL domain values which bypassed type input or a default coercion.
 * Skip evaluated defaults: checking them twice could repeat a domain CHECK
 * expression. PostgreSQL owns the cached constraint state in es_query_cxt. */
void check_load_domain_nulls(load_context& ctx) {
    for (auto& column : ctx.domain_columns) {
        const auto index = static_cast<std::size_t>(column.attribute - 1);
        if (!ctx.slot->tts_isnull[index]
            || (!ctx.assigned_columns[index] && column.has_default)) continue;
        ctx.current_column_name = NameStr(TupleDescAttr(ctx.parent_td, index)->attname);
        bioseq::postgres::support::invoke_pg_as_cpp([&ctx, &column] {
            // The output-pointer cache is the PostgreSQL domain_check API;
            // ownership remains with the load executor's query context.
            domain_check(Datum{0}, true, column.type, &column.constraint_cache, ctx.estate->es_query_cxt);
        });
        ctx.current_column_name = nullptr;
    }
}

/** Check NOT NULL and CHECK constraints against original values before writing. */
void check_load_constraints(load_context& ctx,
        const bioseq::storage::pending_sequence_source* primary,
        const bioseq::storage::pending_sequence_source* quality) {
    if (!ctx.result_rel_info || !ctx.parent_rel->rd_att->constr) return;
    const auto descriptor = ctx.parent_rel->rd_att;
    bool needs_sequences = descriptor->constr->num_check != 0;
    if (!needs_sequences && descriptor->constr->has_not_null) {
        // A NOT NULL failure formats the complete row before raising 23502.
        // Promote staged locators for that diagnostic, but leave successful
        // NOT NULL-only rows on the existing path without source reads.
        for (std::size_t index = 0; index < static_cast<std::size_t>(descriptor->natts); ++index) {
            if (TupleDescAttr(descriptor, index)->attnotnull && ctx.slot->tts_isnull[index]) {
                needs_sequences = true;
                break;
            }
        }
    }
    with_load_pending_sequences(ctx, primary, quality, needs_sequences,
        [&ctx] {
            bioseq::postgres::support::invoke_pg_as_cpp([&ctx] {
                ExecConstraints(ctx.result_rel_info, ctx.slot, ctx.estate);
            });
        });
}

/** Insert ordinary index entries and queue AFTER/constraint-trigger rechecks.
 * The slot already carries the parent row's real TID. PostgreSQL evaluates
 * expression/partial indexes and handles immediate or deferred uniqueness. */
void index_load_row(load_context& ctx,
        const bioseq::storage::pending_sequence_source* primary,
        const bioseq::storage::pending_sequence_source* quality) {
    if (!ctx.result_rel_info) return;
    // AFTER-trigger WHEN expressions are evaluated while events are queued.
    const bool needs_sequences = ctx.indexes_read_sequences
        || (ctx.parent_rel->trigdesc && ctx.parent_rel->trigdesc->trig_insert_after_row);
    with_load_pending_sequences(ctx, primary, quality, needs_sequences, [&ctx] {
        bioseq::postgres::support::invoke_pg_as_cpp([&ctx] {
            List* recheck_indexes = NIL;
            if (ctx.result_rel_info->ri_NumIndices != 0) {
#if PG_VERSION_NUM >= 190000
                recheck_indexes = ExecInsertIndexTuples(
                    ctx.result_rel_info, ctx.estate, 0, ctx.slot, NIL, nullptr);
#else
                recheck_indexes = ExecInsertIndexTuples(
                    ctx.result_rel_info, ctx.slot, ctx.estate, false, false, nullptr, NIL, false);
#endif
            }
            ExecARInsertTriggers(ctx.estate, ctx.result_rel_info, ctx.slot, recheck_indexes, nullptr);
            list_free(recheck_indexes);
        });
    });
}

/// Convert parsed header fields into PostgreSQL values and write completed rows.
struct pg_identifier_writer {
    /// Active load context that outlives this writer.
    load_context& ctx;
    /// Conversion settings borrowed for each parser destination.
    std::span<const dest_coercion> dest;
    /// Defaults applied to destination columns still set to NULL.
    std::span<const default_col> defaults;
    /// Canonical fields use deterministic typed scalar spelling, including raw hex bytea.
    bool canonical_fields{};

    /// Clear the row slot and set every column to SQL NULL.
    void begin_tuple() {
        ctx.assigned_columns.assign(static_cast<std::size_t>(ctx.parent_td->natts), false);
        bioseq::postgres::support::invoke_pg_as_cpp([this] {
            bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.clear_all_to_null();
        });
    }

    /** Reject canonical keys which collapse onto one already populated destination. */
    void check_canonical_assignment(idp::destination_slot slot) const {
        if (canonical_fields && ctx.assigned_columns[dest[slot].attno - 1]
            && std::find(ctx.routing.identifier_slots.begin(), ctx.routing.identifier_slots.end(), slot)
                == ctx.routing.identifier_slots.end())
            raise_load_error_as_cpp(ERRCODE_INVALID_COLUMN_REFERENCE,
                "canonical metadata assigns destination column \"" + dest[slot].column_name
                    + "\" more than once; use exact column names or explicit mappings");
    }

    /// Set one mapped column to SQL NULL.
    /// @param slot Parser destination number, starting at zero.
    void set_null(idp::destination_slot slot) {
        check_canonical_assignment(slot);
        ctx.assigned_columns[dest[slot].attno - 1] = true;
        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_null(dest[slot].attno);
    }
    /// Convert one parsed field to its PostgreSQL column type.
    /// @param slot Parser destination number, starting at zero.
    /// @param data Borrowed field bytes, which need not end with NUL.
    /// @param len Field length  in bytes.
    /// @throws postgres_error If conversion or allocation fails.
    void set_field(idp::destination_slot slot, const char* data, std::int32_t len) {
        if (len < 0 || (len != 0 && (data == nullptr || std::memchr(data, '\0', static_cast<std::size_t>(len)) != nullptr)))
            raise_load_error_as_cpp(ERRCODE_INVALID_TEXT_REPRESENTATION,
                "header/metadata field contains NUL or an invalid byte length");
        if (len != 0) bioseq::postgres::support::invoke_pg_as_cpp([data, len] {
            static_cast<void>(pg_verify_mbstr(GetDatabaseEncoding(), data, len, false));
        });
        const dest_coercion& dc = dest[slot];
        check_canonical_assignment(slot);
        ctx.assigned_columns[dc.attno - 1] = true;
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
        } else if (canonical_fields || dc.kind == dest_kind::text_input) {
            // Headers retain their exact whitespace through domain/name/varchar
            // input; only the existing non-text external-field adapter trims.
            const bioseq::postgres::support::memory_context_scope scope{per_tuple};
            const auto type = TupleDescAttr(ctx.parent_td, dc.attno - 1)->atttypid;
            v = bioseq::postgres::storage::io::parse_io_scalar(
                {data, static_cast<std::size_t>(len)}, type, dc.input_fn, dc.io_param, dc.typmod);
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
    void end_tuple() { end_tuple(nullptr, nullptr); }
    void end_tuple(const bioseq::storage::pending_sequence_source* primary,
                   const bioseq::storage::pending_sequence_source* quality) {
        // Apply defaults to columns still set to NULL. Keep values allocated by default
        // expressions alive until write_column_row has copied them
        for (const default_col& d : defaults) {
            if (ctx.assigned_columns[d.attno - 1] || !ctx.slot->tts_isnull[d.attno - 1]) {
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
        check_load_domain_nulls(ctx);
        check_load_constraints(ctx, primary, quality);
        // FASTA can supply a declared quality DEFAULT. Persist that ordinary
        // SQL value through the same fragment manager as INSERT, so it has a
        // source epoch and participates in normal symbol_counts maintenance.
        std::vector<bioseq::storage::pending_sequence_source> default_sources;
        std::vector<std::pair<AttrNumber, Datum>> default_sequence_values;
        for (const default_col& d : defaults) {
            const auto* attribute = TupleDescAttr(ctx.parent_td, d.attno - 1);
            if (ctx.slot->tts_isnull[d.attno - 1] || attribute->atttypid != load_seq_type_oid()
                || attribute->atttypmod < 0 || !is_quality_alphabet_code(
                    compact_sequence_stdc_type_params::decode(attribute->atttypmod).alphabet_code)) continue;
            auto* flat = bioseq::postgres::support::invoke_pg_as_cpp([this, &d] {
                const auto previous = MemoryContextSwitchTo(get_column_write_per_tuple_context(ctx.write_state));
                auto* value = reinterpret_cast<bioseq::pg_types::seq*>(
                    PG_DETOAST_DATUM_COPY(ctx.slot->tts_values[d.attno - 1]));
                MemoryContextSwitchTo(previous);
                return value;
            });
            if (bioseq::storage::is_locator_stub(flat)) continue; // Actual FASTQ field is already staged.
            auto pending = bioseq::postgres::support::invoke_pg_as_cpp([this, &d, flat] {
                return bioseq::storage::bioseq_default_storage_manager::insert_payload_as_fragments(
                    ctx.main_relid, ctx.main_storage_id, ctx.db_oid, ctx.spc_oid, ctx.main_relfilenode,
                    d.attno, ctx.parent_td, flat, ctx.cap_bytes, ctx.caches);
            });
            const auto locator = pending.locator();
            auto* stub = bioseq::postgres::support::invoke_pg_as_cpp([this, flat, locator] {
                return bioseq::storage::alloc_locator_stub_in(get_column_write_per_tuple_context(ctx.write_state),
                    flat->alphabet_code, flat->storage_unit_byte_width, flat->padding_letters,
                    locator.logical_length, locator, flat->encoding, flat->space);
            });
            ctx.slot->tts_values[d.attno - 1] = PointerGetDatum(stub);
            default_sources.push_back(std::move(pending));
            default_sequence_values.emplace_back(d.attno, PointerGetDatum(flat));
        }
        const auto row_number = write_column_row(ctx.write_state, ctx.slot->tts_values, ctx.slot->tts_isnull);
        bioseq::postgres::support::invoke_pg_as_cpp([this, row_number] {
            ctx.slot->tts_tableOid = ctx.main_relid;
            ctx.slot->tts_tid = row_number_to_tid(row_number);
        });
        // Parent storage keeps locators; index expressions see complete DEFAULT
        // values and bounded staged views for the streamed input columns.
        for (const auto& [attribute, value] : default_sequence_values)
            ctx.slot->tts_values[attribute - 1] = value;
        index_load_row(ctx, primary, quality);
        for (auto& pending : default_sources) pending.publish_after_parent_row();
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
                      bioseq::storage::pending_sequence_source* pending_source,
                      bioseq::storage::pending_sequence_source* quality_source = nullptr,
                      std::uint16_t quality_padding = 0,
                      bool header_is_null = false,
                      bool canonical = false,
                      const idp::canonical_header* parsed_canonical = nullptr) {
    bioseq::pg_types::seq* stub = nullptr;
    if (pending_source) {
    const auto locator = pending_source->locator();
    stub = bioseq::postgres::support::invoke_pg_as_cpp(
        [write_state = ctx.write_state, alphabet_code, byte_width, last_pad, total_bp, locator] {
            return bioseq::storage::alloc_locator_stub_in(
                get_column_write_per_tuple_context(write_state),
                alphabet_code,
                byte_width,
                last_pad,
                total_bp,
                locator);
        });
    }

    ++ctx.record_no;
    load_error_context_scope error_context{ctx, header};

    tuple_writer.begin_tuple(); // Begin with all columns set to NULL.
    ctx.assigned_columns[ctx.cols.seq_attno - 1] = true;
    if (pending_source) bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
        ctx.cols.seq_attno, bioseq::postgres::support::pg_datum::from_pointer(stub));
    if (ctx.cols.quality_attno && (canonical || quality_source))
        ctx.assigned_columns[ctx.cols.quality_attno - 1] = true;
    if (quality_source) {
        auto* quality_stub = bioseq::postgres::support::invoke_pg_as_cpp([&ctx, quality_source, quality_padding] {
            return bioseq::storage::alloc_locator_stub_in(get_column_write_per_tuple_context(ctx.write_state),
                ctx.cols.quality_alphabet_code, ctx.cols.quality_byte_width, quality_padding,
                quality_source->locator().logical_length, quality_source->locator());
        });
        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
            ctx.cols.quality_attno, bioseq::postgres::support::pg_datum::from_pointer(quality_stub));
    }

    tuple_writer.canonical_fields = canonical;
    if (ctx.parser_active) {
        auto routed = parsed_canonical
            ? idp::route_canonical_header(*parsed_canonical, ctx.routing, ctx.writer_ref, header_is_null)
            : idp::parse_and_route_header(ctx.effective_fmt, header, ctx.routing, ctx.parse_state, ctx.writer_ref);
        if (!routed) {
            raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, routed.error());
        }
    } else if (ctx.cols.header_attno != 0 && header_is_null) {
        ctx.assigned_columns[ctx.cols.header_attno - 1] = true;
    } else if (ctx.cols.header_attno != 0) {
        if (header.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                                    "FASTA/FASTQ header exceeds INT_MAX");
        }
        text* htext = bioseq::postgres::support::invoke_pg_as_cpp([&ctx, header] {
            static_cast<void>(pg_verify_mbstr(GetDatabaseEncoding(), header.data(),
                static_cast<int>(header.size()), false));
            const MemoryContext old =
                MemoryContextSwitchTo(get_column_write_per_tuple_context(ctx.write_state));
            text* result = cstring_to_text_with_len(header.data(), static_cast<int>(header.size()));
            MemoryContextSwitchTo(old);
            return result;
        });
        bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
            ctx.cols.header_attno, bioseq::postgres::support::pg_datum::from_pointer(htext));
    }
    tuple_writer.end_tuple(pending_source, quality_source); // Check both staged values before publication.
    if (quality_source) inject_load_failure(2);
    if (pending_source) pending_source->publish_after_parent_row();
    if (quality_source) inject_load_failure(3);
    if (quality_source) quality_source->publish_after_parent_row();
}
