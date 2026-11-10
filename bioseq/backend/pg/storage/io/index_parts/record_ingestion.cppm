/**
 * @file stream_dispatch.cppm
 * @brief Choose the sequence reader and stream its records into PostgreSQL storage.
 */
module;
// Include Highway  and PostgreSQL headers before the named module declaration.
// Imported sequence modules also depend on these declarations.
#include <bioseq/pg_compat.h>
#include <algorithm>
#include <expected>
#include <boost/container/small_vector.hpp>
#include <boost/integer.hpp>
#include <array>
#include <charconv>
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
#include <utility>
#include <vector>
extern "C" {
#include "postgres.h"

#include "access/detoast.h"
#include "access/relation.h"     // index_open / index_close
#include "access/table.h"        // table_open / table_close
#include "access/xact.h"         // CommandCounterIncrement
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"   // TEXTOID, VARCHAROID, BPCHAROID
#include "commands/sequence.h"   // ResetSequenceCaches
#include "executor/spi.h"
#include "executor/executor.h"   // ExecConstraints, ExecInitExpr, ExecEvalExpr
#include "executor/tuptable.h"   // TupleTableSlot, MakeTupleTableSlot, TTSOpsVirtual, Exec*Tuple
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/portal.h"
#include "nodes/params.h"
#include "nodes/makefuncs.h"     // makeNode (ResultRelInfo)
#include "optimizer/optimizer.h" // expression_planner
#include "utils/array.h"         // deconstruct_array, ARR_ELEMTYPE
#include "utils/builtins.h"      // text_to_cstring, cstring_to_text_with_len, regtypein
#include "utils/lsyscache.h"     // getTypeInputInfo, get_typtype, getBaseType
#include "utils/rel.h"           // RelationGetDescr
#include "utils/resowner.h"      // CurrentResourceOwner, TopTransactionResourceOwner
#include "utils/snapmgr.h" // Snapshot stack operations.
#include "utils/jsonb.h"
#include "utils/typcache.h" // lookup_rowtype_tupdesc

#include "fmgr.h"
#include "rewrite/rewriteHandler.h" // build_column_default
}

export module bioseq.postgres.storage.io:record_ingestion;

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

import bioseq.common.seqan_adapter;
import bioseq.simd.utils;
import bioseq.sequences.rank_pack;
import bioseq.sequences.cigar;
import bioseq.storage.lazy_sequence;
import bioseq.io.record_schema;
import bioseq.io.alignment.sequence_codec;
import bioseq.io.alignment.record_codec;
import bioseq.io.identifier_parser.preservation;
import bioseq.parallelism.memory_admission;

/** Exact rank correspondence built once without instantiating an alphabet cross product. */
[[nodiscard]] std::array<std::uint32_t, 256> load_rank_mapping(std::uint8_t source_code,
        std::uint8_t destination_code) {
    std::array<std::uint32_t, 256> mapping;
    mapping.fill(255u);
    if (source_code >= pgseq_supported_alphabets::alphabet_count
        || destination_code >= pgseq_supported_alphabets::alphabet_count)
        raise_load_error_as_cpp(ERRCODE_DATATYPE_MISMATCH, "unknown imported alphabet");
    std::array<std::int16_t, 256> symbols;
    symbols.fill(-32768);
    bool source_quality = false;
    bool source_protein = false;
    bool source_legacy = false;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(source_code,
        [&symbols, &source_quality, &source_legacy, &source_protein]<class Alphabet>() {
            if constexpr (bioseq::sequences::quality_alphabet<Alphabet>) {
                source_quality = true;
                source_legacy = bioseq::sequences::quality_alphabet_traits<Alphabet>::legacy;
                for (std::size_t rank = 0; rank < seqan3::alphabet_size<Alphabet>; ++rank)
                    symbols[rank] = seqan3::to_phred(Alphabet{}.assign_rank(rank));
            } else if constexpr (!bioseq::cigar_operation_alphabet<Alphabet>) {
                source_protein = seqan3::aminoacid_alphabet<Alphabet>;
                for (std::size_t rank = 0; rank < seqan3::alphabet_size<Alphabet>; ++rank)
                    symbols[rank] = static_cast<unsigned char>(seqan3::to_char(Alphabet{}.assign_rank(rank)));
            }
        });
    pgseq_supported_alphabets::dispatch_alphabet_by_code(destination_code,
        [&mapping, &symbols, source_quality, source_legacy, source_protein]<class Alphabet>() {
            if constexpr (!bioseq::cigar_operation_alphabet<Alphabet>) {
                constexpr bool quality = bioseq::sequences::quality_alphabet<Alphabet>;
                if (quality != source_quality || (!quality && source_protein != seqan3::aminoacid_alphabet<Alphabet>)) return;
                if constexpr (quality) {
                    if (source_legacy != bioseq::sequences::quality_alphabet_traits<Alphabet>::legacy) return;
                }
                for (std::size_t rank = 0; rank < seqan3::alphabet_size<Alphabet>; ++rank) {
                    const auto symbol = Alphabet{}.assign_rank(rank);
                    const auto value = [&symbol]() -> std::int16_t {
                        if constexpr (quality) return seqan3::to_phred(symbol);
                        else return static_cast<unsigned char>(seqan3::to_char(symbol));
                    }();
                    for (std::size_t source_rank = 0; source_rank < symbols.size(); ++source_rank)
                        if (symbols[source_rank] == value) mapping[source_rank] = static_cast<std::uint32_t>(rank);
                }
            }
        });
    return mapping;
}

/** One unpublished fragment owner and its flat locator stub. */
struct staged_load_sequence {
    bioseq::storage::pending_sequence_source pending;
    bioseq::pg_types::seq* stub{};
};

/** Repack only bounded rank tiles into the existing transactional fragment writer. */
template<class Alphabet, std::size_t Width>
[[nodiscard]] staged_load_sequence stage_loaded_sequence(load_context& ctx,
        const bioseq::storage::borrowed_rank_window_source& source, AttrNumber attno,
        std::span<const std::uint32_t> mapping) {
    using unit = typename boost::uint_t<Width * 8>::least;
    using configuration = sequence_custom_alphabet<Alphabet, unit>;
    using traits = typename compact_sequence_range<configuration>::traits;
    constexpr std::size_t rank_capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
    constexpr std::size_t tile_letters = rank_capacity / traits::letters_per_unit * traits::letters_per_unit;
    constexpr std::size_t unit_capacity = rank_capacity / traits::letters_per_unit + 1;
    std::array<std::uint8_t, rank_capacity> read_scratch;
    std::array<std::uint8_t, rank_capacity> converted;
    std::array<unit, unit_capacity> words;
    const auto cardinality = pgseq_supported_alphabets::cardinality_for_code(source.alphabet_code);
    const auto transform = bioseq::io::alignment::sequence_codec_kernel();
    if (!cardinality || !transform)
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "import rank conversion is unavailable");
    bioseq::storage::sequence_stream_writer writer;
    struct abort_scope {
        bioseq::storage::sequence_stream_writer& writer;
        /** Release provisional storage on every normal or error exit. */
        ~abort_scope() { writer.abort(); }
    } guard{writer};
    constexpr auto alphabet_code = pgseq_supported_alphabets::template index_of<Alphabet>();
    writer.begin(ctx.main_relid, ctx.db_oid, ctx.spc_oid, ctx.main_relfilenode,
        ctx.main_storage_id, attno, ctx.parent_td, alphabet_code, static_cast<std::uint8_t>(Width),
        ctx.cap_bytes, ctx.caches);
    std::uint16_t padding{};
    for (std::uint64_t offset = 0; offset < source.symbol_count;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(tile_letters, source.symbol_count - offset));
        const auto ranks = source.read_checked(offset, count, read_scratch);
        if (!ranks) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "cannot read imported sequence rank tile");
        std::span<const std::uint8_t> selected = *ranks;
        if (source.alphabet_code != alphabet_code) {
            if (!transform(bioseq::simd::sequence_rank_transform::map_bytes, selected, *cardinality,
                    mapping.first(*cardinality), std::span{converted}.first(count)))
                raise_load_error_as_cpp(ERRCODE_DATATYPE_MISMATCH, "input contains symbols absent from destination alphabet");
            selected = std::span{converted}.first(count);
        }
        using unit_sink = bioseq::sequences::fixed_rank_pack_unit_sink<unit>;
        bioseq::sequences::checked_rank_pack_sink<configuration,
            bioseq::sequences::invalid_rank_policy::reject,
            bioseq::sequences::pack_capacity_policy::fixed_span, unit_sink> packer{unit_sink{words}};
        const auto appended = bioseq::simd::active_target::append_native_ranks_chunk(selected, packer);
        const auto packed = packer.finish(count);
        if (appended != bioseq::sequences::rank_pack_status::ok || !packed)
            raise_load_error_as_cpp(ERRCODE_DATATYPE_MISMATCH, "input contains symbols absent from destination alphabet");
        writer.append_packed_bytes({reinterpret_cast<const std::uint8_t*>(words.data()),
            static_cast<std::size_t>(packed.written_units) * sizeof(unit)}, count);
        padding = packed.padding_letters;
        offset += count;
    }
    auto pending = writer.finish(source.symbol_count, padding);
    auto* stub = bioseq::postgres::support::invoke_pg_as_cpp([&ctx, &source, padding, locator = pending.locator()] {
        return bioseq::storage::alloc_locator_stub_in(get_column_write_per_tuple_context(ctx.write_state),
            alphabet_code, static_cast<std::uint8_t>(Width), padding, source.symbol_count, locator);
    });
    return {std::move(pending), stub};
}

/** Select packing width independently of the source alphabet. */
[[nodiscard]] staged_load_sequence stage_loaded_sequence(load_context& ctx,
        const bioseq::storage::borrowed_rank_window_source& source, AttrNumber attno,
        std::uint8_t destination_alphabet, std::uint8_t width) {
    std::span<const std::uint32_t> mapping;
    if (source.alphabet_code != destination_alphabet) {
        const auto key = std::pair{source.alphabet_code, destination_alphabet};
        auto found = ctx.rank_mappings.find(key);
        if (found == ctx.rank_mappings.end())
            found = ctx.rank_mappings.emplace(key, load_rank_mapping(source.alphabet_code, destination_alphabet)).first;
        mapping = found->second;
    }
    std::optional<staged_load_sequence> result;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(destination_alphabet,
        [&ctx, &source, attno, width, &mapping, &result]<class Alphabet>() {
            if constexpr (!bioseq::cigar_operation_alphabet<Alphabet>)
                dispatch_storage_unit_byte_width(width,
                    [&ctx, &source, attno, &mapping, &result]<std::size_t Width>() {
                        result.emplace(stage_loaded_sequence<Alphabet, Width>(ctx, source, attno, mapping));
                    });
        });
    if (!result) raise_load_error_as_cpp(ERRCODE_DATATYPE_MISMATCH, "CIGAR cannot be a biological or quality destination");
    return std::move(*result);
}

/** Canonicalize imported CIGAR runs into the flat ABI using two bounded passes. */
[[nodiscard]] Datum load_cigar_datum(const bioseq::borrowed_cigar_run_source& source) {
    // Reuse canonical append with one pending run, including across source-tile boundaries.
    const auto visit_canonical = [&source](auto&& consume) -> std::expected<void, bioseq::cigar_error> {
        std::array<bioseq::cigar_run, 1> pending_storage{};
        bioseq::cigar_run_span_sink<bioseq::cigar_symbol> pending{pending_storage};
        bioseq::borrowed_cigar_cursor cursor{source};
        for (;;) {
            const auto run = cursor.next();
            if (!run) return std::unexpected(run.error());
            if (!*run) break;
            if (!pending.empty() && pending.back().operation != (**run).operation) {
                if (const auto consumed = consume(pending.back()); !consumed) return consumed;
                pending.used = 0;
            }
            if (const auto appended = bioseq::append_canonical_run(pending, **run); !appended) return appended;
        }
        return pending.empty() ? std::expected<void, bioseq::cigar_error>{} : consume(pending.back());
    };
    std::uint64_t count{};
    const auto measured = visit_canonical([&count](bioseq::cigar_run) -> std::expected<void, bioseq::cigar_error> {
        if (++count > std::numeric_limits<std::uint32_t>::max())
            raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED, "imported CIGAR has too many runs");
        return {};
    });
    if (!measured) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "cannot normalize imported CIGAR");
    auto* result = bioseq::postgres::support::invoke_pg_as_cpp([count] {
        return allocate_pg_cigar_payload(static_cast<std::uint32_t>(count), bioseq::cigar_space::compatibility);
    });
    bioseq::cigar_payload_writer<bioseq::cigar_symbol> writer{
        {reinterpret_cast<std::byte*>(result->data), static_cast<std::size_t>(VARSIZE(result)) - bioseq::pg_types::seq::header_bytes()},
        static_cast<std::uint32_t>(count), bioseq::cigar_space::compatibility};
    const auto written = visit_canonical([&writer](bioseq::cigar_run run) { return writer.append(run); });
    if (!written) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "invalid imported CIGAR");
    const auto finished = writer.finish();
    if (!finished) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "invalid imported CIGAR termination");
    result->bp_size = *finished;
    return PointerGetDatum(result);
}

/** Bounded, admitted scalar serialization before PostgreSQL copies JSONB. */
struct load_json_sink {
    std::string text;
    bioseq::parallelism::memory_permit permit;
    /** Admit retained capacity before appending canonical scalar bytes. */
    [[nodiscard]] std::expected<void, std::string> append(std::string_view bytes) {
        if (bytes.size() > MaxAllocSize - text.size()) return std::unexpected("imported JSON exceeds PostgreSQL value limit");
        const auto wanted = text.size() + bytes.size();
        if (wanted > text.capacity()) {
            const auto capacity = std::min<std::size_t>(MaxAllocSize, std::max(wanted, std::max<std::size_t>(256, text.capacity() * 2)));
            if (!permit) {
                auto admitted = bioseq::parallelism::memory_admission_controller::instance().try_reserve(capacity);
                if (!admitted) return std::unexpected("cannot admit imported JSON");
                permit = std::move(*admitted);
            } else if (!permit.grow_to(capacity)) return std::unexpected("cannot grow imported JSON");
            text.reserve(capacity);
        }
        text.append(bytes);
        return {};
    }
};

/** Form the generated full alignment composite in the current per-row PostgreSQL context. */
[[nodiscard]] Datum load_alignment_datum(const bioseq::io::export_record_view& row) {
    using codec = bioseq::postgres::storage::io::sql_row_codecs::alignment_codec;
    bioseq::storage::load_pg::composite_rows::alignment value{};
    const auto& source = *row.alignment;
    const auto integer = [](std::optional<std::uint64_t> number) -> std::optional<std::int64_t> {
        if (!number) return std::nullopt;
        if (*number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            raise_load_error_as_cpp(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, "alignment field exceeds SQL bigint");
        return static_cast<std::int64_t>(*number);
    };
    const auto string = [](std::string_view bytes) -> text* {
        if (bytes.size() > MaxAllocSize - VARHDRSZ || bytes.find('\0') != std::string_view::npos)
            raise_load_error_as_cpp(ERRCODE_INVALID_TEXT_REPRESENTATION, "alignment text exceeds PostgreSQL limits");
        return bioseq::postgres::support::invoke_pg_as_cpp([bytes] {
            return cstring_to_text_with_len(bytes.data(), static_cast<int>(bytes.size()));
        });
    };
    if (source.flags) value.flag = *source.flags;
    if (source.mapping_quality) value.mapq = *source.mapping_quality;
    if (row.qname) value.qname = string(*row.qname);
    value.template_id = integer(row.identity.template_id);
    value.segment_id = integer(row.identity.segment_id);
    value.reference_id = integer(row.identity.reference_id);
    if (!source.reference_name.empty()) value.reference_name = string(source.reference_name);
    value.reference_length = integer(source.reference_length);
    value.target_start = integer(source.target_start); value.target_end = integer(source.target_end);
    value.query_length = integer(source.query_length); value.query_start = integer(source.query_start); value.query_end = integer(source.query_end);
    if (!source.mate_reference.empty()) value.mate_reference = string(source.mate_reference);
    value.mate_start = integer(source.mate_start); value.template_length = source.template_length;
    value.header_context_id = integer(source.header_context_id);
    value.source_orientation = string(source.sequence_orientation == bioseq::io::record_orientation::reference_forward
        ? "reference_forward" : "original_query");
    value.cigar_form = string(source.form == bioseq::io::cigar_form::sam_complete ? "sam_complete" : "alignment_core");
    if (source.cigar) value.cigar = load_cigar_datum(*source.cigar);
    load_json_sink sink;
    bioseq::io::identifier_parser::preservation_encoder encoder{sink};
    encoder.auxiliary(source.auxiliary);
    if (const auto serialized = encoder.result(); !serialized)
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, serialized.error());
    value.aux = bioseq::postgres::support::invoke_pg_as_cpp([&sink] {
        return DirectFunctionCall1(jsonb_in, CStringGetDatum(sink.text.c_str()));
    });
    const auto oid = bioseq::postgres::support::invoke_pg_as_cpp([] { return bioseq_type_oid_missing_ok("alignment"); });
    auto* descriptor = bioseq::postgres::support::invoke_pg_as_cpp([oid] { return lookup_rowtype_tupdesc(oid, -1); });
    auto encoded = bioseq::postgres::support::invoke_pg_as_cpp([&value] {
        return codec::encode(value, CurrentMemoryContext);
    });
    const auto tuple = bioseq::postgres::support::invoke_pg_as_cpp([&encoded, descriptor] {
        return encoded.form_tuple(descriptor);
    });
    bioseq::postgres::support::invoke_pg_as_cpp([descriptor] { ReleaseTupleDesc(descriptor); });
    return HeapTupleGetDatum(tuple);
}

/** Insert one validated decoded record through the existing parent/fragment transaction. */
void insert_loaded_record(load_context& ctx, const bioseq::io::export_record_view& row) {
    pg_identifier_writer tuple_writer{ctx, ctx.dest, ctx.defaults};
    ++ctx.record_no;
    load_error_context_scope error_context{ctx, row.header.value_or(std::string_view{})};
    tuple_writer.begin_tuple();
    ctx.writer_ref = idp::make_identifier_writer_reference(tuple_writer);
    std::optional<staged_load_sequence> sequence;
    std::optional<staged_load_sequence> quality;
    if (row.sequence) sequence.emplace(stage_loaded_sequence(ctx, *row.sequence, ctx.cols.seq_attno,
        ctx.cols.alphabet_code, ctx.cols.byte_width));
    if (row.quality) {
        if (!ctx.cols.quality_attno) raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION, "quality destination is missing");
        quality.emplace(stage_loaded_sequence(ctx, *row.quality, ctx.cols.quality_attno,
            ctx.cols.quality_alphabet_code, ctx.cols.quality_byte_width));
    }
    ctx.assigned_columns[ctx.cols.seq_attno - 1] = true;
    if (sequence) bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
        ctx.cols.seq_attno, bioseq::postgres::support::pg_datum::from_pointer(sequence->stub));
    if (ctx.cols.quality_attno) ctx.assigned_columns[ctx.cols.quality_attno - 1] = true;
    if (quality) bioseq::postgres::support::pg_slot_datums_view{ctx.slot}.set_raw(
        ctx.cols.quality_attno, bioseq::postgres::support::pg_datum::from_pointer(quality->stub));
    tuple_writer.canonical_fields = true;
    if (row.header && (row.header->size() > MaxAllocSize - VARHDRSZ || row.header->find('\0') != std::string_view::npos))
        raise_load_error_as_cpp(ERRCODE_INVALID_TEXT_REPRESENTATION, "imported header exceeds PostgreSQL text limits");
    for (const auto destination : ctx.routing.identifier_slots) {
        if (row.header) tuple_writer.set_field(destination, row.header->data(), static_cast<std::int32_t>(row.header->size()));
        else tuple_writer.set_null(destination);
    }
    for (const auto& field : row.metadata) {
        const auto value = field.is_null ? std::nullopt : std::optional<std::string_view>{field.bytes};
        if (const auto routed = idp::route_canonical_field(field.name, value, ctx.routing, ctx.writer_ref); !routed)
            raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, routed.error());
    }
    if (ctx.cols.alignment_attno) {
        ctx.assigned_columns[ctx.cols.alignment_attno - 1] = true;
        if (row.alignment) {
            const bioseq::postgres::support::memory_context_scope scope{
                get_column_write_per_tuple_context(ctx.write_state)};
            ctx.slot->tts_values[ctx.cols.alignment_attno - 1] = load_alignment_datum(row);
            ctx.slot->tts_isnull[ctx.cols.alignment_attno - 1] = false;
        }
    }
    tuple_writer.end_tuple(sequence ? &sequence->pending : nullptr, quality ? &quality->pending : nullptr);
    if (sequence) sequence->pending.publish_after_parent_row();
    if (quality) quality->pending.publish_after_parent_row();
}

/** Resolve only actually requested BAM '=' references into bounded backend recipes. */
class load_reference_lookup {
    std::optional<Oid> relation_;
    pg_sequence_read_session* session_{};
    bioseq::parallelism::memory_permit permit_;
    std::string query_;

    /** Resolve a readable reference identifier with the loader's shared header policy.
     * A single text primary key wins, then a unique header alias, then the sole
     * text column. All names borrow the open relation until resolution finishes.
     */
    [[nodiscard]] static AttrNumber identifier_attribute(Relation relation) {
        const auto primary_attribute = bioseq::postgres::support::invoke_pg_as_cpp([relation]() -> AttrNumber {
            const auto primary_oid = bioseq::postgres::support::find_primary_key_oid(relation);
            if (!OidIsValid(primary_oid)) return InvalidAttrNumber;
            const auto primary = index_open(primary_oid, AccessShareLock);
            const AttrNumber attribute = primary->rd_index && primary->rd_index->indnkeyatts == 1
                ? primary->rd_index->indkey.values[0] : InvalidAttrNumber;
            index_close(primary, AccessShareLock);
            return attribute;
        });
        const auto descriptor = RelationGetDescr(relation);
        std::vector<idp::column_descriptor> columns;
        std::vector<AttrNumber> attributes;
        for (int index = 0; index < descriptor->natts; ++index) {
            const auto* attribute = TupleDescAttr(descriptor, index);
            if (attribute->attisdropped || attribute->attnum <= 0) continue;
            if (!load_type_accepts_header(attribute->atttypid)) continue;
            columns.push_back({.name = NameStr(attribute->attname), .is_text = true,
                .is_primary_key = attribute->attnum == primary_attribute,
                .not_null = attribute->attnotnull, .has_default = attribute->atthasdef,
                .is_identity_or_generated = attribute->attidentity != '\0' || attribute->attgenerated != '\0'});
            attributes.push_back(attribute->attnum);
        }
        const auto mapping = idp::resolve_identifier_mapping(idp::header_format::automatic,
            idp::header_format::plain, columns, {});
        if (!mapping) raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION,
            "reference_table identifier: " + mapping.error());
        if (mapping->routing.identifier_slots.size() != 1u)
            raise_load_error_as_cpp(ERRCODE_INVALID_TABLE_DEFINITION,
                "reference_table requires one unambiguous text identifier");
        return attributes[mapping->slot_to_column[mapping->routing.identifier_slots.front()]];
    }

public:
    /** Delay all reference-table work until an '=' symbol requires it. */
    explicit load_reference_lookup(std::optional<Oid> relation) : relation_(relation) {}
    load_reference_lookup(const load_reference_lookup&) = delete;
    /** Release retained sequence handles on the backend after all reader workers drain. */
    ~load_reference_lookup() { abort_pg_sequence_read_session_noexcept(session_); }

    /** Read a uniquely named reference through normal SELECT permissions and RLS. */
    [[nodiscard]] std::expected<std::optional<bioseq::storage::borrowed_rank_window_source>, std::string>
    find(std::string_view name) {
        if (!relation_) return std::optional<bioseq::storage::borrowed_rank_window_source>{};
        if (name.size() > MaxAllocSize - VARHDRSZ || name.find('\0') != std::string_view::npos)
            return std::unexpected("invalid reference lookup name");
        if (query_.empty()) {
            const auto relation = bioseq::postgres::support::invoke_pg_as_cpp([this] {
                return table_open(*relation_, AccessShareLock);
            });
            try {
                const auto columns = resolve_load_columns(relation);
                const auto header_attribute = identifier_attribute(relation);
                query_ = bioseq::postgres::support::invoke_pg_as_cpp([relation, columns, header_attribute] {
                    const auto descriptor = RelationGetDescr(relation);
                    const auto* sequence = quote_identifier(NameStr(TupleDescAttr(descriptor, columns.seq_attno - 1)->attname));
                    const auto* header = quote_identifier(NameStr(TupleDescAttr(descriptor, header_attribute - 1)->attname));
                    const auto* table = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(relation)),
                        RelationGetRelationName(relation));
                    return std::string{"SELECT "} + sequence + " FROM " + table + " WHERE " + header + " = $1 LIMIT 2";
                });
            } catch (...) {
                bioseq::postgres::support::invoke_pg_as_cpp([relation] { table_close(relation, NoLock); });
                throw;
            }
            bioseq::postgres::support::invoke_pg_as_cpp([relation] { table_close(relation, NoLock); });
            session_ = bioseq::postgres::support::invoke_pg_as_cpp([] {
                return begin_pg_sequence_read_session(CurrentMemoryContext);
            });
        }
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            if (SPI_connect() != SPI_OK_CONNECT) ereport(ERROR, (errmsg("cannot open reference lookup SPI context")));
        });
        std::optional<bioseq::storage::borrowed_rank_window_source> result;
        Portal portal = nullptr;
        try {
            bioseq::postgres::support::invoke_pg_as_cpp([this, name, &portal] {
                // Own every analyzed/planned node and parameter in the portal.
                // No saved plan may reparse the name after this identity check.
                portal = CreateNewPortal();
                const auto previous = MemoryContextSwitchTo(portal->portalContext);
                const auto* query_text = pstrdup(query_.c_str());
                auto* parsed = pg_parse_query(query_text);
                if (list_length(parsed) != 1)
                    elog(ERROR, "reference lookup requires exactly one SELECT");
                const Oid parameter_type = TEXTOID;
                auto* rewritten = pg_analyze_and_rewrite_fixedparams(
                    static_cast<RawStmt*>(linitial(parsed)), query_text, &parameter_type, 1, nullptr);
                if (list_length(rewritten) != 1)
                    elog(ERROR, "reference lookup must rewrite to one SELECT");
                auto* query = static_cast<Query*>(linitial(rewritten));
                if (query->commandType != CMD_SELECT || query->hasModifyingCTE
                    || query->utilityStmt || query->rowMarks != NIL)
                    elog(ERROR, "reference lookup must be a read-only SELECT");
                if (!bioseq::postgres::support::query_has_single_from_relation(*query, *relation_))
                    ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("bioseq.load could not preserve the reference_table relation identity"),
                        errhint("Retry the same regclass request after concurrent schema changes finish.")));
                auto* statement = pg_plan_query(query, query_text, CURSOR_OPT_NO_SCROLL, nullptr
#if PG_VERSION_NUM >= 190000
                    , nullptr
#endif
                );
                if (!CommandIsReadOnly(statement))
                    elog(ERROR, "reference lookup planned a non-read-only command");
                auto* parameters = makeParamList(1);
                parameters->params[0].value = PointerGetDatum(cstring_to_text_with_len(
                    name.data(), static_cast<int>(name.size())));
                parameters->params[0].isnull = false;
                parameters->params[0].pflags = PARAM_FLAG_CONST;
                parameters->params[0].ptype = TEXTOID;
                PortalDefineQuery(portal, nullptr, query_text, CMDTAG_SELECT, list_make1(statement), nullptr);
                portal->cursorOptions = CURSOR_OPT_NO_SCROLL;
                MemoryContextSwitchTo(previous);
                // ExecutorStart performs normal caller ACL checks; the verified
                // rewritten Query retains the caller's row-security policies.
                PortalStart(portal, parameters, 0, GetActiveSnapshot());
            });
            const auto value = bioseq::postgres::support::invoke_pg_as_cpp([portal]() -> std::optional<Datum> {
                SPI_cursor_fetch(portal, true, 2);
                if (SPI_processed > 1) ereport(ERROR, (errcode(ERRCODE_CARDINALITY_VIOLATION),
                    errmsg("reference_table has more than one row for requested reference")));
                if (SPI_processed == 0) return std::nullopt;
                bool is_null{};
                const auto datum = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &is_null);
                return is_null ? std::nullopt : std::optional{datum};
            });
            if (value) {
                const auto bytes = bioseq::postgres::support::invoke_pg_as_cpp([value] {
                    return std::uint64_t{8192} + (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(*value))
                        ? std::uint64_t{0} : static_cast<std::uint64_t>(toast_raw_datum_size(*value)));
                });
                if (bytes > std::numeric_limits<std::uint64_t>::max() - permit_.bytes())
                    raise_load_error_as_cpp(ERRCODE_PROGRAM_LIMIT_EXCEEDED, "reference reader admission overflows");
                const auto wanted = bytes + permit_.bytes();
                if (!permit_) {
                    auto admitted = bioseq::parallelism::memory_admission_controller::instance().try_reserve(wanted);
                    if (!admitted) raise_load_error_as_cpp(ERRCODE_OUT_OF_MEMORY, "cannot admit reference read owner");
                    permit_ = std::move(*admitted);
                } else if (!permit_.grow_to(wanted))
                    raise_load_error_as_cpp(ERRCODE_OUT_OF_MEMORY, "cannot admit reference read owner");
                result = retain_pg_rank_source(*session_, *value);
            }
            // Drop the exact executor before SPI frees its fetched tuple table.
            // Clear our pointer first: an error during PortalDrop remains owned
            // by PostgreSQL's resource cleanup and must not be closed twice.
            const auto completed = std::exchange(portal, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp([completed] { SPI_cursor_close(completed); });
        } catch (...) {
            const auto failure = std::current_exception();
            if (portal) {
                try { bioseq::postgres::support::invoke_pg_as_cpp([portal] { SPI_cursor_close(portal); }); } catch (...) {}
            }
            try { bioseq::postgres::support::invoke_pg_as_cpp([] { SPI_finish(); }); } catch (...) {}
            std::rethrow_exception(failure);
        }
        bioseq::postgres::support::invoke_pg_as_cpp([] { SPI_finish(); });
        return result;
    }
};

/** Borrow one quality field window without copying or converting the original sequence. */
struct load_quality_window {
    const bioseq::storage::borrowed_rank_window_source& original;
    std::uint64_t offset{};
    std::size_t length{};
    /** Shift a checked local read into its original owner. */
    [[nodiscard]] static const std::uint8_t* read(const void* context, std::uint64_t from,
            std::size_t count, std::span<std::uint8_t> scratch) {
        const auto& window = *static_cast<const load_quality_window*>(context);
        if (from > window.length || count > window.length - from) return nullptr;
        const auto ranks = window.original.read_checked(window.offset + from, count, scratch);
        return ranks ? ranks->data() : nullptr;
    }
    /** Retain the original alphabet and orientation; only logical coordinates change. */
    [[nodiscard]] bioseq::storage::borrowed_rank_window_source borrow() const {
        return {this, &read, length, original.alphabet_code, original.orientation, false};
    }
};

/** Validate canonical quality projection before replacing it with preserved original ranks. */
void validate_preserved_text_quality(std::optional<bioseq::storage::borrowed_rank_window_source> original,
        std::span<const std::uint8_t> native, std::uint64_t offset) {
    if (!original) {
        std::array<std::uint8_t, bioseq::simd::sequence_rank_io_chunk_capacity> missing;
        missing.fill('!');
        while (!native.empty()) {
            const auto count = std::min(native.size(), missing.size());
            if (bioseq::simd::first_unequal_sequence_rank(native.data(), missing.data(), count) != count)
                raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "canonical missing quality must use numeric-zero wire placeholders");
            native = native.subspan(count);
        }
        return;
    }
    if (offset > original->symbol_count || native.size() > original->symbol_count - offset)
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, "canonical quality projection exceeds original length");
    const load_quality_window window{*original, offset, native.size()};
    const auto source = window.borrow();
    bioseq::io::alignment::native_projection_comparison_sink comparison{native};
    std::expected<std::size_t, std::string> result = std::unexpected("preserved quality has a non-quality alphabet");
    pgseq_supported_alphabets::dispatch_alphabet_by_code(original->alphabet_code,
        [&comparison, &source, &result]<class Alphabet>() {
            if constexpr (bioseq::sequences::quality_alphabet<Alphabet>)
                result = bioseq::io::alignment::emit_quality<Alphabet, false, true>(comparison, source, false);
        });
    if (!result) raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, result.error());
    if (const auto matched = comparison.finish(); !matched)
        raise_load_error_as_cpp(ERRCODE_DATA_EXCEPTION, matched.error());
}
