/**
 * @file streamed_writes.cppm
 * @brief Fragmented sequence streaming writes and source checksums.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"
#include "utils/resowner.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/indexing.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/vacuum.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "utils/tuplestore.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.manager:sequence_storage_streamed_writes;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.storage;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.table.io;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.indexing.storage;
import bioseq.common.seqan_adapter;  // ceil_log2 used in letters_per_unit_for
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
// The PG-free bucket math and sequence_locator value live in their own
// module so the unit-test TU can link them without the Postgres backend.
// Re-exported here so existing `import bioseq_storage` consumers are unchanged.
import bioseq.storage.layout;
import :sequence_storage_catalog;
import :sequence_storage_layout_cache;
import :sequence_storage_catalog_ddl;

namespace bioseq::storage {

// insert_sequence_streamed write hot path.
//
// One sequence → ceil(total_bp / fragment_bp) sub-table rows. Producer is
// invoked once per fragment in a per-fragment MemoryContext that is reset
// between fragments so peak RSS is bounded by one fragment's worth of state
// Per-sequence shard-rollover pre-allocation keeps a sequence's fragments in a
// single shard so the locator's
// shard_idx is a single value.
// ---------------------------------------------------------------------------

namespace detail {

/// Feed the exact logical symbol bytes used by the existing revision-3
/// independent checksum oracle. Fragment-sized decoding keeps memory bounded
/// while moving the O(N) work to insertion rather than a cold index read.
inline void update_canonical_source_checksum(
        bioseq::indexing::crc64_ecma182_accumulator& checksum,
        const bioseq::pg_types::seq* fragment) {
    if (fragment == nullptr || is_locator_stub(fragment)) {
        source_version_detail::source_catalog_error(
            "canonical checksum received a non-fragment value");
    }
    fragment->convert_to([&checksum]<class p_alphabet>(
            const compact_sequence_range<p_alphabet>& range) {
        const std::string decoded =
            ::bioseq::simd::active_target::decode_sequence_simd<p_alphabet>(
                range);
        if (decoded.size() != range.bp_size()) {
            source_version_detail::source_catalog_error(
                "canonical checksum decoded length disagrees");
        }
        checksum.update(std::string_view{decoded});
    });
}

}  // namespace detail

export template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
template<class p_fragment_producer>
pending_sequence_source
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::insert_sequence_streamed(
    Oid user_relid, std::uint64_t main_storage_id,
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_storage_unit_byte_width,
    sequence_size_t total_bp_size,
    std::uint16_t last_fragment_padding_letters,
    sequence_size_t current_cap_bytes,
    seq_caches* cache,
    p_fragment_producer&& producer)
{
    if (!OidIsValid(user_relid) || main_storage_id == 0) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq storage: source epoch requires parent relation/storage identity");
    }
    if (cache == nullptr || cache->side != seq_caches::access_side::write) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq storage: insert_sequence_streamed requires a write-side seq_caches");
    }
    if (cache->main_database_oid == InvalidOid) {
        cache->main_database_oid   = db_oid;
        cache->main_tablespace_oid = spc_oid;
        cache->main_relfilenode    = main_relfilenode;
    }

    const auto alpha_code = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code,
                                                         logical_storage_unit_byte_width);
    if (lpu == 0) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: insert_sequence_streamed could not derive "
                "letters_per_unit (alphabet_code={}, byte_width={})",
                static_cast<unsigned>(alpha_code),
                static_cast<unsigned>(logical_storage_unit_byte_width)));
    }
    const sequence_size_t frag_bp =
        static_cast<sequence_size_t>(detail::k_fragment_storage_units) * lpu;
    const sequence_size_t num_fragments =
        total_bp_size == 0 ? 0u : (total_bp_size + frag_bp - 1) / frag_bp;
    if (num_fragments > static_cast<sequence_size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: fragment count {} exceeds UINT32_MAX",
                num_fragments));
    }

    // Per-row varlena bytes for a full (= non-last) fragment.
    const std::size_t frag_varlena_bytes =
        bioseq::pg_types::seq::header_bytes()
        + detail::k_fragment_storage_units
              * static_cast<std::size_t>(logical_storage_unit_byte_width);
    // Total varlena bytes the sequence occupies in the shard (= sum of
    // fragment-row sizes). Used for the per-shard cap accounting; each row
    // has the same size because the last fragment row is also
    // allocated at `frag_varlena_bytes` (palloc0 leaves trailing units 0).
    const sequence_size_t total_varlena_bytes =
        static_cast<sequence_size_t>(num_fragments) *
        static_cast<sequence_size_t>(frag_varlena_bytes);

    const std::int16_t bucket_idx =
        static_cast<std::int16_t>(sequence_bucket_index(total_bp_size));

    const std::uint64_t source_epoch =
        sequence_source_version_manager::allocate_epoch();
    bioseq::indexing::crc64_ecma182_accumulator source_checksum{};

    if (num_fragments == 0) {
        sequence_locator empty_locator{};
        empty_locator.payload_version = sequence_locator_payload_version;
        empty_locator.bucket_index = static_cast<std::uint8_t>(bucket_idx);
        empty_locator.shard_index = 0;
        empty_locator.fragment_count = 0;
        empty_locator.logical_length = 0;
        empty_locator.first_fragment_row = 0;
        empty_locator.source_epoch = source_epoch;
        sequence_source_version_manager::insert_unpublished(
            sequence_source_version_row{
                .source_epoch = source_epoch,
                .user_relid = user_relid,
                .database_oid = db_oid,
                .tablespace_oid = spc_oid,
                .main_relfilenode = main_relfilenode,
                .main_storage_id = main_storage_id,
                .column_attno = column_attno,
                .locator = empty_locator,
                .fragment_relid = InvalidOid,
                .fragment_storage_id = 0,
                .alphabet_code = logical_alphabet_code,
                .storage_unit_byte_width = logical_storage_unit_byte_width,
                .padding_letters = last_fragment_padding_letters,
                .source_content_checksum = source_checksum.checksum(),
                .state = k_sequence_source_state_unpublished,
            });
        return pending_sequence_source{empty_locator};
    }

    // Per-sequence shard pre-allocation. If the active shard
    // cannot fit the WHOLE sequence's bytes, allocate a fresh shard up front.
    Relation shard = open_or_create_shard(db_oid, spc_oid, main_relfilenode,
                                          column_attno, parent_tupdesc, bucket_idx,
                                          RowExclusiveLock, cache);

    auto active = bioseq::postgres::support::invoke_pg_as_cpp(
        [db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx] {
            return sequence_sub_table_manager::lookup_active_shard(
                db_oid, spc_oid, main_relfilenode, column_attno, bucket_idx);
        });
    if (!active.has_value()) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            "bioseq storage: active shard disappeared mid-insert");
    }
    const std::uint64_t active_key =
        seq_caches::key_for(column_attno, bucket_idx, active->shard_idx);
    sequence_size_t pending = 0;
    if (auto it = cache->entries.find(active_key); it != cache->entries.end()) {
        pending = it->second.pending_bytes_delta;
    }
    const sequence_size_t projected =
        active->bytes_written + pending + total_varlena_bytes;
    // Pre-allocate a fresh shard when the active one cannot fit the whole
    // sequence. This follows the normal per-row rollover semantics,
    // but the unit of accounting is the whole sequence now.
    const bool rollover_required =
        active->bytes_written + pending > 0
        && (current_cap_bytes == 0 || projected > current_cap_bytes);

    std::int32_t shard_idx = 0;
    if (rollover_required) {
        if (auto it = cache->entries.find(active_key);
            it != cache->entries.end() && it->second.pending_bytes_delta != 0)
        {
            const std::int32_t active_shard_idx = active->shard_idx;
            const sequence_size_t pending_bytes =
                it->second.pending_bytes_delta;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [db_oid, spc_oid, main_relfilenode, column_attno,
                 bucket_idx, active_shard_idx, pending_bytes,
                 current_cap_bytes] {
                    (void) sequence_sub_table_manager::update_shard_bytes_written(
                        db_oid, spc_oid, main_relfilenode,
                        column_attno, bucket_idx, active_shard_idx,
                        pending_bytes, current_cap_bytes);
                });
            it->second.pending_bytes_delta = 0;
            it->second.pending_total_payload_delta = 0;
        } else {
            const std::int32_t active_shard_idx = active->shard_idx;
            const sequence_size_t effective_cap =
                current_cap_bytes == 0
                ? sequence_size_t{1}
                : current_cap_bytes;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [db_oid, spc_oid, main_relfilenode, column_attno,
                 bucket_idx, active_shard_idx, effective_cap] {
                    (void) sequence_sub_table_manager::update_shard_bytes_written(
                        db_oid, spc_oid, main_relfilenode,
                        column_attno, bucket_idx, active_shard_idx,
                        /*delta_bytes=*/0, effective_cap);
                });
        }
        shard = allocate_new_shard_and_open(db_oid, spc_oid, main_relfilenode,
                                            column_attno, parent_tupdesc, bucket_idx,
                                            active->shard_idx + 1,
                                            RowExclusiveLock, cache);
        shard_idx = active->shard_idx + 1;
    } else {
        shard_idx = active->shard_idx;
    }

    // Refuse to write a sequence whose total bytes
    // exceed the cap (would not fit any shard, including a freshly-allocated
    // one). current_cap_bytes==0 is the sentinel "no cap, one shard per
    // sequence" and the check is suppressed.
    if (current_cap_bytes > 0 && total_varlena_bytes > current_cap_bytes) {
        storage_error_as_cpp(
            ERRCODE_INTERNAL_ERROR,
            fmt::format(
                "bioseq storage: sequence with {} bytes exceeds shard cap "
                "{} bytes ({} GiB). Raise "
                "bioseq.storage.sequence_shard.max_size_gib.",
                total_varlena_bytes, current_cap_bytes,
                current_cap_bytes >> 30));
    }

    const std::uint64_t cache_key =
        seq_caches::key_for(column_attno, bucket_idx, shard_idx);
    seq_caches::entry& entry = cache->entries[cache_key];
    entry.rel = shard;

    if (entry.write_state == nullptr) {
        const SubTransactionId current_subtransaction =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentSubTransactionId(); });
        entry.write_state = bioseq::postgres::support::invoke_pg_as_cpp(
            [shard, current_subtransaction] {
                return initialize_column_write_state(
                    shard, RelationGetDescr(shard),
                    RelationGetRelid(shard),
                    current_subtransaction);
            });
    }

    // Serialize writers on this parent, column, bucket, and shard
    // shard) so the row numbers write_column_row assigns are CONTIGUOUS
    // within one sequence's fragments.
    const std::int64_t row_lock_key =
          (static_cast<std::int64_t>(db_oid)                << 48)
        ^ (static_cast<std::int64_t>(main_relfilenode)      << 16)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(column_attno))    << 8)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(bucket_idx))      << 4)
        ^  static_cast<std::int64_t>(shard_idx);
    bioseq::postgres::support::invoke_pg_as_cpp([row_lock_key] {
        (void) DirectFunctionCall1(
            pg_advisory_xact_lock_int8, Int64GetDatum(row_lock_key));
    });

    // Use separate scratch and per-fragment contexts. The producer
    // may allocate long-lived state in `scratch_mctx`; per-fragment palloc
    // happens in `per_fragment_mctx` which is RESET between fragments to
    // keep peak RSS bounded.
    std::uint64_t first_sub_row_no = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t last_sub_row_no  = std::numeric_limits<std::uint64_t>::max();
    MemoryContext scratch_mctx = nullptr;
    MemoryContext per_fragment_mctx = nullptr;
    std::exception_ptr fragment_failure;
    try {
        const MemoryContext parent_mctx = CurrentMemoryContext;
        scratch_mctx = bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx] {
            return AllocSetContextCreate(
                parent_mctx, "bioseq insert_sequence scratch",
                ALLOCSET_DEFAULT_SIZES);
        });
        per_fragment_mctx =
            bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx] {
                return AllocSetContextCreate(
                    parent_mctx, "bioseq insert_sequence per-fragment",
                    ALLOCSET_DEFAULT_SIZES);
            });

        for (sequence_size_t fragment_no = 0;
             fragment_no < num_fragments;
             ++fragment_no) {
            bioseq::pg_types::seq* frag =
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [per_fragment_mctx, frag_varlena_bytes] {
                        MemoryContextReset(per_fragment_mctx);
                        const MemoryContext previous =
                            MemoryContextSwitchTo(per_fragment_mctx);
                        auto* result =
                            static_cast<bioseq::pg_types::seq*>(
                                palloc0(frag_varlena_bytes));
                        MemoryContextSwitchTo(previous);
                        return result;
                    });
            const bool is_last = (fragment_no + 1 == num_fragments);
            bioseq::postgres::support::memory_context_scope frag_ctx{
                per_fragment_mctx};

            SET_VARSIZE(frag, frag_varlena_bytes);
            frag->alphabet_code =
                static_cast<bioseq::pg_types::seq::alphabet_code_t>(
                    logical_alphabet_code);
            frag->storage_unit_byte_width =
                logical_storage_unit_byte_width;
            // Producer fills bp_size + padding_letters + data[] for fragment `n`.
            std::forward<p_fragment_producer>(producer)(
                static_cast<std::size_t>(fragment_no),
                frag,
                scratch_mctx,
                is_last);

            // Cross-check the producer's bp_size / padding consistency.
            const sequence_size_t expected_bp = is_last
                ? (total_bp_size - fragment_no * frag_bp)
                : frag_bp;
            if (static_cast<sequence_size_t>(frag->bp_size)
                != expected_bp) {
                storage_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    fmt::format(
                        "bioseq storage: producer bp_size mismatch on "
                        "fragment {} (got {}, expected {})",
                        fragment_no,
                        static_cast<sequence_size_t>(frag->bp_size),
                        expected_bp));
            }
            if (is_last
                && frag->padding_letters
                    != last_fragment_padding_letters) {
                storage_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    fmt::format(
                        "bioseq storage: producer last-fragment "
                        "padding_letters mismatch (got {}, expected {})",
                        static_cast<unsigned>(frag->padding_letters),
                        static_cast<unsigned>(
                            last_fragment_padding_letters)));
            }
            if (!is_last && frag->padding_letters != 0) {
                storage_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    fmt::format(
                        "bioseq storage: producer non-last "
                        "padding_letters must be 0 (fragment {}, got {})",
                        fragment_no,
                        static_cast<unsigned>(frag->padding_letters)));
            }

            detail::update_canonical_source_checksum(
                source_checksum, frag);

            Datum values[1] = {PointerGetDatum(frag)};
            bool nulls[1] = {false};
            const std::uint64_t row_no =
                write_column_row(
                    entry.write_state, values, nulls);
            if (fragment_no == 0) {
                first_sub_row_no = row_no;
            } else if (row_no != last_sub_row_no + 1) {
                storage_error_as_cpp(
                    ERRCODE_INTERNAL_ERROR,
                    fmt::format(
                        "bioseq storage: non-contiguous row numbers "
                        "during streamed insert (expected {}, got {})",
                        last_sub_row_no + 1, row_no));
            }
            last_sub_row_no = row_no;
        }
    } catch (...) {
        fragment_failure = std::current_exception();
    }

    const auto delete_context =
        [&fragment_failure](MemoryContext& context) {
            const MemoryContext doomed =
                std::exchange(context, nullptr);
            if (doomed == nullptr) return;
            try {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [doomed] { MemoryContextDelete(doomed); });
            } catch (...) {
                if (fragment_failure == nullptr) {
                    fragment_failure = std::current_exception();
                }
            }
        };
    delete_context(per_fragment_mctx);
    delete_context(scratch_mctx);
    if (fragment_failure != nullptr) {
        std::rethrow_exception(fragment_failure);
    }

    entry.pending_bytes_delta         += total_varlena_bytes;
    entry.pending_total_payload_delta += total_varlena_bytes;

    if (num_fragments == 0) {
        // Empty sequence — no rows were written. Choose a deterministic row
        // number (0) as the stub's first_sub_row_no; the scanner short-
        // circuits when num_fragments == 0 and never reads this row.
        first_sub_row_no = 0;
    }

    sequence_locator out{};
    out.payload_version          = sequence_locator_payload_version;
    out.bucket_index       = static_cast<std::uint8_t>(bucket_idx);
    out.reserved_16           = 0;
    out.shard_index        = static_cast<std::uint32_t>(shard_idx);
    out.fragment_count    = static_cast<std::uint32_t>(num_fragments);
    out.reserved_32           = 0;
    out.logical_length    = total_bp_size;
    out.first_fragment_row = first_sub_row_no;
    out.source_epoch     = source_epoch;

    const Oid fragment_relid = RelationGetRelid(shard);
    const std::uint64_t fragment_storage_id =
        bioseq::postgres::support::invoke_pg_as_cpp([shard] {
            return column_storage_id(shard, false);
        });
    sequence_source_version_manager::insert_unpublished(
        sequence_source_version_row{
            .source_epoch = source_epoch,
            .user_relid = user_relid,
            .database_oid = db_oid,
            .tablespace_oid = spc_oid,
            .main_relfilenode = main_relfilenode,
            .main_storage_id = main_storage_id,
            .column_attno = column_attno,
            .locator = out,
            .fragment_relid = fragment_relid,
            .fragment_storage_id = fragment_storage_id,
            .alphabet_code = logical_alphabet_code,
            .storage_unit_byte_width = logical_storage_unit_byte_width,
            .padding_letters = last_fragment_padding_letters,
            .source_content_checksum = source_checksum.checksum(),
            .state = k_sequence_source_state_unpublished,
        });
    return pending_sequence_source{out};
}

export template<class p_length_type,
         std::size_t p_bucket_minimum_length_bits,
         std::size_t p_bucket_maximum_length_bits>
pending_sequence_source
sequence_storage_manager<
    p_length_type,
    p_bucket_minimum_length_bits,
    p_bucket_maximum_length_bits>::insert_payload_as_fragments(
    Oid user_relid, std::uint64_t main_storage_id,
    Oid db_oid, Oid spc_oid, Oid main_relfilenode,
    std::int16_t column_attno, TupleDesc parent_tupdesc,
    const bioseq::pg_types::seq* original_hdr,
    sequence_size_t current_cap_bytes,
    seq_caches* cache)
{
    if (original_hdr == nullptr) {
        bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: insert_payload_as_fragments received null original_hdr");
    }
    if (is_locator_stub(original_hdr)) {
        bioseq_log<logging_level::error, logging_target::postgres>( "bioseq storage: insert_payload_as_fragments received a stub; "
               "the caller must screen out stubs before re-inserting");
    }

    const sequence_size_t total_bp_size =
        static_cast<sequence_size_t>(original_hdr->bp_size);
    const std::uint8_t logical_alphabet_code =
        static_cast<std::uint8_t>(original_hdr->alphabet_code);
    const std::uint8_t logical_byte_width =
        static_cast<std::uint8_t>(original_hdr->storage_unit_byte_width);
    const std::uint16_t last_fragment_padding =
        static_cast<std::uint16_t>(original_hdr->padding_letters);

    const auto alpha_code = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha_code, logical_byte_width);
    if (lpu == 0) {
        bioseq_log<logging_level::error, logging_target::postgres>(
               "bioseq storage: insert_payload_as_fragments could not derive "
               "letters_per_unit (alphabet_code={}, byte_width={})",
               static_cast<unsigned>(alpha_code),
               static_cast<unsigned>(logical_byte_width));
    }
    const sequence_size_t frag_bp =
        static_cast<sequence_size_t>(detail::k_fragment_storage_units) * lpu;
    const sequence_size_t num_fragments =
        total_bp_size == 0 ? 0u : (total_bp_size + frag_bp - 1) / frag_bp;
    const std::size_t bytes_per_unit =
        static_cast<std::size_t>(logical_byte_width);
    const std::size_t fragment_bytes =
        detail::k_fragment_storage_units * bytes_per_unit;

    // Slice the original payload into N fragment rows.
    const std::uint8_t* const src_data =
        reinterpret_cast<const std::uint8_t*>(original_hdr->data);
    const std::size_t src_bytes =
        static_cast<std::size_t>(VARSIZE(original_hdr)) - bioseq::pg_types::seq::header_bytes();

    auto producer = [src_data, src_bytes, fragment_bytes, frag_bp, total_bp_size,
                     last_fragment_padding](
        std::size_t fragment_no, bioseq::pg_types::seq* dst, MemoryContext /*scratch*/,
        bool is_last) noexcept
    {
        const std::size_t src_offset = fragment_no * fragment_bytes;
        const std::size_t copy_bytes = std::min(fragment_bytes,
            src_offset < src_bytes ? src_bytes - src_offset : std::size_t{0});
        if (copy_bytes > 0) {
            std::memcpy(dst->data, src_data + src_offset, copy_bytes);
        }
        // Trailing bytes already zero from palloc0.
        const sequence_size_t this_fragment_bp = is_last
            ? (total_bp_size - static_cast<sequence_size_t>(fragment_no) * frag_bp)
            : frag_bp;
        dst->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(this_fragment_bp);
        dst->padding_letters = is_last ? last_fragment_padding : std::uint16_t{0};
    };

    return insert_sequence_streamed(
        user_relid, main_storage_id,
        db_oid, spc_oid, main_relfilenode, column_attno, parent_tupdesc,
        logical_alphabet_code, logical_byte_width,
        total_bp_size, last_fragment_padding, current_cap_bytes, cache,
        producer);
}

// ---------------------------------------------------------------------------
// sequence_stream_writer implementation. Mirrors the per-fragment
// row write of insert_sequence_streamed, but driven incrementally: the total
// length is unknown until finish(), so the shard is opened lazily and the cap
// is checked as the sequence streams. No cross-shard rollover (one shard per
// sequence); a single sequence over the cap raises ERROR and the transaction
// rollback cleans the partial fragment rows.
// ---------------------------------------------------------------------------
void sequence_stream_writer::begin(
    Oid user_relid, Oid db_oid, Oid spc_oid,
    RelFileNumber main_relfilenode, std::uint64_t main_storage_id,
    AttrNumber seq_attno, TupleDesc parent_td,
    std::uint8_t alphabet_code, std::uint8_t byte_width,
    sequence_size_t cap_bytes, seq_caches* caches)
{
    if (caches == nullptr || caches->side != seq_caches::access_side::write) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: sequence_stream_writer requires a write-side "
            "seq_caches");
    }
    if (!OidIsValid(user_relid) || main_storage_id == 0) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: source epoch requires parent relation/storage identity");
    }
    user_relid_ = user_relid;
    db_oid_ = db_oid; spc_oid_ = spc_oid; main_relfilenode_ = main_relfilenode;
    main_storage_id_ = main_storage_id;
    column_attno_ = static_cast<std::int16_t>(seq_attno);
    parent_td_ = parent_td;
    alphabet_code_ = alphabet_code; byte_width_ = byte_width;
    cap_bytes_ = cap_bytes; caches_ = caches;

    if (caches_->main_database_oid == InvalidOid) {
        caches_->main_database_oid   = db_oid;
        caches_->main_tablespace_oid = spc_oid;
        caches_->main_relfilenode    = main_relfilenode;
    }

    const auto alpha = static_cast<bioseq::pg_types::seq::alphabet_code_t>(alphabet_code);
    const std::size_t lpu = detail::letters_per_unit_for(alpha, byte_width);
    if (lpu == 0) {
        storage_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            fmt::format(
                "bioseq.load: could not derive letters_per_unit "
                "(alphabet_code={}, byte_width={})",
                static_cast<unsigned>(alpha),
                static_cast<unsigned>(byte_width)));
    }
    frag_payload_bytes_ = detail::k_fragment_storage_units
        * static_cast<std::size_t>(byte_width);
    frag_varlena_bytes_ = bioseq::pg_types::seq::header_bytes() + frag_payload_bytes_;
    frag_bp_ = static_cast<sequence_size_t>(detail::k_fragment_storage_units)
        * lpu;

    buffer_.clear();
    buffer_.reserve(frag_payload_bytes_ + 256);
    first_sub_row_no_ = std::numeric_limits<std::uint64_t>::max();
    last_sub_row_no_  = std::numeric_limits<std::uint64_t>::max();
    num_fragments_ = 0;
    total_varlena_bytes_ = 0;
    source_checksum_value_ = 0;
    source_epoch_ = sequence_source_version_manager::allocate_epoch();
    bucket_idx_ = 0;
    shard_idx_ = 0;
    cache_key_ = 0;
    shard_ = nullptr;
    write_state_ = nullptr;
    active_ = true;
}

void sequence_stream_writer::open_shard() {
    if (shard_ != nullptr) return;
    // All streamed sequences land in the top length bucket: the size is unknown
    // until finish(), the bucket is purely organisational (the locator records
    // it; reads use the locator), and co-locating a load's fragments keeps
    // sequential-read locality.
    bucket_idx_ = static_cast<std::int16_t>(
        bioseq_default_storage_manager::sequence_bucket_index(
            std::numeric_limits<sequence_size_t>::max()));
    shard_ = open_or_create_shard(
        db_oid_, spc_oid_, main_relfilenode_, column_attno_, parent_td_,
        bucket_idx_, RowExclusiveLock, caches_);
    auto active = bioseq::postgres::support::invoke_pg_as_cpp([this] {
        return sequence_sub_table_manager::lookup_active_shard(
            db_oid_, spc_oid_, main_relfilenode_,
            column_attno_, bucket_idx_);
    });
    if (!active.has_value()) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: active shard disappeared mid-load");
    }
    shard_idx_ = active->shard_idx;
    cache_key_ = seq_caches::key_for(column_attno_, bucket_idx_, shard_idx_);
    seq_caches::entry& entry = caches_->entries[cache_key_];
    entry.rel = shard_;
    if (entry.write_state == nullptr) {
        const SubTransactionId subtransaction =
            bioseq::postgres::support::invoke_pg_as_cpp(
                [] { return GetCurrentSubTransactionId(); });
        entry.write_state = initialize_column_write_state(
            shard_, RelationGetDescr(shard_), RelationGetRelid(shard_),
            subtransaction);
    }
    write_state_ = entry.write_state;
    // Serialize writers on this parent, column, bucket, and shard
    // so the row numbers write_column_row assigns stay contiguous within one
    // sequence's fragments (and across the sequences of one load).
    const std::int64_t row_lock_key =
          (static_cast<std::int64_t>(db_oid_)            << 48)
        ^ (static_cast<std::int64_t>(main_relfilenode_)  << 16)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(column_attno_)) << 8)
        ^ (static_cast<std::int64_t>(
               static_cast<std::uint16_t>(bucket_idx_))   << 4)
        ^  static_cast<std::int64_t>(shard_idx_);
    bioseq::postgres::support::invoke_pg_as_cpp([row_lock_key] {
        (void) DirectFunctionCall1(
            pg_advisory_xact_lock_int8, Int64GetDatum(row_lock_key));
    });
}

void sequence_stream_writer::write_fragment(
    const std::uint8_t* payload, std::size_t payload_bytes,
    sequence_size_t bp, std::uint16_t pad)
{
    open_shard();
    if (cap_bytes_ > 0 &&
        total_varlena_bytes_ + frag_varlena_bytes_ > cap_bytes_) {
        storage_error_as_cpp(
            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            fmt::format(
                "bioseq.load: sequence exceeds shard cap {} bytes ({} GiB) "
                "at fragment {}. Raise "
                "bioseq.storage.sequence_shard.max_size_gib.",
                total_varlena_bytes_ + frag_varlena_bytes_,
                cap_bytes_ >> 30, num_fragments_));
    }
    // Keep the allocation in a checked leaf so palloc cannot longjmp across
    // caller-owned checksum/state objects. write_column_row provides its
    // own checked writer leaf below.
    bioseq::pg_types::seq* frag = bioseq::postgres::support::invoke_pg_as_cpp([this] {
        const MemoryContext old = MemoryContextSwitchTo(
            get_column_write_per_tuple_context(write_state_));
        bioseq::pg_types::seq* result = static_cast<bioseq::pg_types::seq*>(
            palloc0(frag_varlena_bytes_));
        MemoryContextSwitchTo(old);
        return result;
    });
    SET_VARSIZE(frag, frag_varlena_bytes_);
    frag->alphabet_code =
        static_cast<bioseq::pg_types::seq::alphabet_code_t>(alphabet_code_);
    frag->storage_unit_byte_width = byte_width_;
    frag->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(bp);
    frag->padding_letters = pad;
    if (payload_bytes > 0) {
        std::memcpy(frag->data, payload, payload_bytes);
    }
    bioseq::indexing::crc64_ecma182_accumulator checksum{
        .value = source_checksum_value_};
    detail::update_canonical_source_checksum(checksum, frag);
    source_checksum_value_ = checksum.checksum();
    Datum values[1] = {PointerGetDatum(frag)};
    bool nulls[1] = {false};
    const std::uint64_t row_no =
        write_column_row(write_state_, values, nulls);
    if (num_fragments_ == 0) {
        first_sub_row_no_ = row_no;
    } else if (row_no != last_sub_row_no_ + 1) {
        storage_error_as_cpp(
            ERRCODE_DATA_EXCEPTION,
            fmt::format(
                "bioseq.load: non-contiguous row numbers during streamed "
                "insert (expected {}, got {})",
                last_sub_row_no_ + 1, row_no));
    }
    last_sub_row_no_ = row_no;
    ++num_fragments_;
    total_varlena_bytes_ += frag_varlena_bytes_;
}

void sequence_stream_writer::append_packed_bytes(
    std::span<const std::uint8_t> bytes, sequence_size_t /*bp_count*/)
{
    if (!active_) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: append_packed_bytes before begin()");
    }
    if (bytes.empty()) return;
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    // Write full fragments only while STRICTLY more than one full fragment is
    // buffered, so the final (possibly padded) fragment is always emitted by
    // finish() with the caller's last_pad. Non-final fragments are full units
    // (padding 0, bp = frag_bp_).
    std::size_t off = 0;
    while (buffer_.size() - off > frag_payload_bytes_) {
        write_fragment(buffer_.data() + off, frag_payload_bytes_, frag_bp_, 0);
        off += frag_payload_bytes_;
    }
    if (off > 0) {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(off));
    }
}

pending_sequence_source sequence_stream_writer::finish(
    sequence_size_t total_bp, std::uint16_t last_pad)
{
    if (!active_) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: finish() before begin()");
    }
    // Emit the final fragment (the buffered remainder ≤ one fragment). Its bp is
    // whatever remains after the full fragments already written.
    const sequence_size_t emitted_bp =
        static_cast<sequence_size_t>(num_fragments_) * frag_bp_;
    if (!buffer_.empty()) {
        const sequence_size_t last_bp = total_bp - emitted_bp;
        write_fragment(buffer_.data(), buffer_.size(), last_bp, last_pad);
        buffer_.clear();
    }

    if (num_fragments_ == 0) {
        // Empty sequence — no rows written; the scanner short-circuits on
        // num_fragments == 0 and never reads first_sub_row_no.
        first_sub_row_no_ = 0;
    } else {
        seq_caches::entry& entry = caches_->entries[cache_key_];
        entry.pending_bytes_delta         += total_varlena_bytes_;
        entry.pending_total_payload_delta += total_varlena_bytes_;
    }

    sequence_locator out{};
    out.payload_version          = sequence_locator_payload_version;
    out.bucket_index       = static_cast<std::uint8_t>(bucket_idx_);
    out.reserved_16           = 0;
    out.shard_index        = static_cast<std::uint32_t>(shard_idx_);
    out.fragment_count    = num_fragments_;
    out.reserved_32           = 0;
    out.logical_length    = total_bp;
    out.first_fragment_row = first_sub_row_no_;
    out.source_epoch     = source_epoch_;

    const Oid fragment_relid = num_fragments_ == 0
        ? InvalidOid : RelationGetRelid(shard_);
    const std::uint64_t fragment_storage_id = num_fragments_ == 0
        ? 0 : bioseq::postgres::support::invoke_pg_as_cpp([this] {
            return column_storage_id(shard_, false);
        });
    sequence_source_version_manager::insert_unpublished(
        sequence_source_version_row{
            .source_epoch = source_epoch_,
            .user_relid = user_relid_,
            .database_oid = db_oid_,
            .tablespace_oid = spc_oid_,
            .main_relfilenode = static_cast<Oid>(main_relfilenode_),
            .main_storage_id = main_storage_id_,
            .column_attno = column_attno_,
            .locator = out,
            .fragment_relid = fragment_relid,
            .fragment_storage_id = fragment_storage_id,
            .alphabet_code = alphabet_code_,
            .storage_unit_byte_width = byte_width_,
            .padding_letters = last_pad,
            .source_content_checksum = source_checksum_value_,
            .state = k_sequence_source_state_unpublished,
        });
    active_ = false;
    return pending_sequence_source{out};
}

void sequence_stream_writer::abort() {
    // Transaction rollback reclaims any fragment rows already written; just drop
    // the in-flight buffer + state so a re-begin starts clean.
    buffer_.clear();
    active_ = false;
    source_epoch_ = 0;
}

// ---------------------------------------------------------------------------

}  // namespace bioseq::storage
