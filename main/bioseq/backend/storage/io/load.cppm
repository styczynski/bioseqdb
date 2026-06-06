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
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include "catalog/namespace.h" // (parity w/ sample_table_udf; relid arrives as regclass)
#include "catalog/pg_type_d.h" // TEXTOID, VARCHAROID, BPCHAROID
#include "commands/sequence.h" // ResetSequenceCaches
#include "executor/tuptable.h" // TupleTableSlot, MakeTupleTableSlot, TTSOpsVirtual, Exec*Tuple
#include "utils/builtins.h"    // text_to_cstring, cstring_to_text_with_len, regtypein
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
import bioseq_io.public_api;               // stream_sequences_file, packed_sequence_chunk

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

[[nodiscard]] LoadColumns resolve_load_columns(Relation rel) {
    TupleDesc td = RelationGetDescr(rel);
    const Oid seq_type_oid = load_seq_type_oid();
    if (!OidIsValid(seq_type_oid))
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
            errmsg("bioseq.load: bioseq.seq type not found (is the extension installed?)")));

    std::int16_t header_attno = 0, seq_attno = 0;
    int header_count = 0, seq_count = 0;
    for (int i = 0; i < td->natts; ++i) {
        Form_pg_attribute attr = TupleDescAttr(td, i);
        if (attr->attisdropped || attr->attnum <= 0) continue;
        if (attr->atttypid == seq_type_oid) {
            ++seq_count; seq_attno = static_cast<std::int16_t>(i + 1);
        } else if (attr->atttypid == TEXTOID || attr->atttypid == VARCHAROID
                   || attr->atttypid == BPCHAROID) {
            ++header_count;
            if (header_attno == 0) header_attno = static_cast<std::int16_t>(i + 1);
        }
    }
    // Prompt rules: exactly one seq column; at most one string column.
    if (seq_count == 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
            errmsg("bioseq.load: table has no bioseq.seq column")));
    if (seq_count > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
            errmsg("bioseq.load: table has %d bioseq.seq columns; exactly one is "
                   "required (v1)", seq_count)));
    if (header_count > 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
            errmsg("bioseq.load: table has %d varchar/text columns; at most one "
                   "is allowed (the header column)", header_count)));

    // Direct ColumnarWriteRow bypasses executor defaults/generation. Any
    // ignored column must therefore be nullable and require no default
    // evaluation. Nullable extra columns are written NULL.
    for (int i = 0; i < td->natts; ++i) {
        Form_pg_attribute attr = TupleDescAttr(td, i);
        if (attr->attisdropped || attr->attnum <= 0) continue;
        const std::int16_t attno = static_cast<std::int16_t>(i + 1);
        if (attno == seq_attno || attno == header_attno) continue;
        if (attr->attnotnull || attr->atthasdef ||
            attr->attidentity != '\0' || attr->attgenerated != '\0') {
            ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
                errmsg("bioseq.load: ignored column \"%s\" is NOT NULL, "
                       "defaulted, identity, or generated; direct load only "
                       "supports nullable ignored columns",
                       NameStr(attr->attname))));
        }
    }

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
// §4.4 — load context + main-row write
// --------------------------------------------------------------------------

struct LoadContext {
    Oid main_relid;                                  // parent table OID (for update_index)
    Oid db_oid, spc_oid; RelFileNumber main_relfilenode;
    Relation parent_rel; TupleDesc parent_td;
    LoadColumns cols;
    bioseq::storage::seq_caches* caches;
    ColumnarWriteState* write_state;
    sequence_size_t cap_bytes;
    TupleTableSlot* slot;
};

void write_parent_row(LoadContext& ctx, std::string_view header,
                      std::uint8_t alphabet_code, std::uint8_t byte_width,
                      std::uint16_t last_pad, sequence_size_t total_bp,
                      const bioseq::storage::seq_locator& locator) {
    pgseq_t* stub = bioseq::storage::alloc_locator_stub_in(
        ColumnarWritePerTupleContext(ctx.write_state),
        alphabet_code, byte_width, last_pad, total_bp, locator);

    const int natts = ctx.parent_td->natts;
    ExecClearTuple(ctx.slot);
    for (int i = 0; i < natts; ++i) { ctx.slot->tts_isnull[i] = true; }  // others NULL

    ctx.slot->tts_values[ctx.cols.seq_attno - 1] = PointerGetDatum(stub);
    ctx.slot->tts_isnull[ctx.cols.seq_attno - 1] = false;
    if (ctx.cols.header_attno != 0) {
        // cstring_to_text_with_len copies the (small) header into the per-tuple
        // context (palloc'd, owned by the row). One small copy.
        if (header.size() > static_cast<std::size_t>(INT_MAX))
            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("bioseq.load: FASTA/FASTQ header exceeds INT_MAX")));
        text* htext = cstring_to_text_with_len(header.data(),
                                               static_cast<int>(header.size()));
        ctx.slot->tts_values[ctx.cols.header_attno - 1] = PointerGetDatum(htext);
        ctx.slot->tts_isnull[ctx.cols.header_attno - 1] = false;
    }
    ExecStoreVirtualTuple(ctx.slot);
    (void) ColumnarWriteRow(ctx.write_state, ctx.slot->tts_values, ctx.slot->tts_isnull);
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

void cleanup_load_context(LoadContext& ctx, bool commit) {
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

[[nodiscard]] std::int64_t run_load(Oid relid, const char* path,
                                    bioseq::io::sequence_file_kind kind) {
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
        ctx.cols             = resolve_load_columns(ctx.parent_rel);      // §4.1
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

        n = dispatch_and_stream(ctx, path, kind);                         // §4.2
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
    const Oid   relid = PG_GETARG_OID(0);                 // regclass -> Oid
    text* const path_text = PG_GETARG_TEXT_PP(1);
    char* const path = text_to_cstring(path_text);

    // 1. file kind (NO_POSTGRES) — error -> ereport.
    auto kind_or = bioseq::io::infer_sequence_file_kind(path);
    if (!kind_or)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("bioseq.load: %s", kind_or.error().c_str())));
    const auto kind = *kind_or;

    // 2..6 — open, discover columns, dispatch, stream, build index, close.
    const std::int64_t n = run_load(relid, path, kind);   // §4.1-4.5 orchestration
    JustLog("[bioseq] load: '{}' -> relid={} loaded {} records", path, relid, n);
    PG_RETURN_INT64(n);
}

}  // extern "C"
