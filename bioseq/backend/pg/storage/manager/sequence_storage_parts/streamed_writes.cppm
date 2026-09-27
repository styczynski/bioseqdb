/**
 * @file streamed_writes.cppm
 * @brief Write packed sequences as fragment rows and calculate source checksums.
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
import bioseq.common.seqan_adapter;
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
// Share locator types and bucket calculations with standalone code.
import bioseq.storage.layout;
import :sequence_storage_catalog;
import :sequence_storage_layout_cache;
import :sequence_storage_catalog_ddl;

namespace bioseq::storage {

// Write each sequence as consecutive rows in one fragment table. The
// producer fills one fragment at a time. Its temporary allocations are reset
// between fragments, while separate scratch storage can survive the whole write.
// Choose a shard with room for the complete sequence before writing its rows.

namespace detail {

/// Add a fragment's decoded symbols to the sequence checksum.
/// Decoding one fragment at a time limits temporary storage and saves readers
/// from scanning the complete sequence just to calculate this checksum.
/// @param checksum CRC-64 accumulator to update.
/// @param fragment Packed sequence fragment to decode.
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

/// Store a sequence as consecutive fragment rows in one shard.
///
/// The producer receives the fragment number, an allocated destination, a
/// scratch context and a flag for the last fragment. It fills the packed
/// symbols, length and padding. The destination already has its type.
/// Temporary fragment allocations are cleared between calls.
///
/// @tparam p_length_type Unsigned type used for sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest bucket exponent.
/// @tparam p_fragment_producer Callback that fills one fragment.
/// @param user_relid Parent table.
/// @param main_storage_id Parent storage generation.
/// @param db_oid Parent database.
/// @param spc_oid Parent tablespace.
/// @param main_relfilenode Parent table file number.
/// @param column_attno One-based sequence column number.
/// @param parent_tupdesc Parent column descriptions, kept alive during the call.
/// @param logical_alphabet_code Sequence alphabet code.
/// @param logical_storage_unit_byte_width Storage-unit width in bytes.
/// @param total_bp_size Sequence length in symbols.
/// @param last_fragment_padding_letters Unused positions in the final storage unit.
/// @param current_cap_bytes Shard size limit. Zero gives each sequence a shard
/// without imposing a size limit.
/// @param cache Write cache, kept alive during the call.
/// @param producer Callback that fills each fragment.
/// @return Token holding the locator and unpublished source version.
/// @throws An error if a fragment, its row range or the shard capacity is invalid.
///
/// An advisory lock prevents other writers from interleaving fragment rows.
template<class p_length_type,
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

    // Stored size of each fragment row, including its sequence header.
    const std::size_t frag_varlena_bytes =
        bioseq::pg_types::seq::header_bytes()
        + detail::k_fragment_storage_units
              * static_cast<std::size_t>(logical_storage_unit_byte_width);
    // Charge the shard for all fragment rows, including the zero-filled space
    // in the final partial fragment. Every row has the same allocated size.
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

    // Open a shard, then check whether the complete sequence fits there.
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
    // Start a new shard if this sequence would exceed the current shard limit.
    // With a zero limit, start a new shard for each sequence.
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

    // Every fragment must stay in the same shard. Reject a sequence that exceeds
    // the size limit even in an empty shard. Zero disables this size check.
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

    // Lock this shard so another writer cannot interleave rows among this
    // sequence's fragments. Locators require consecutive fragment row numbers.
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

    // Keep producer state in scratch_mctx across fragments. Reset
    // per_fragment_mctx before each fragment to free its temporary allocations.
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

            // Check that the producer filled the expected length and padding.
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

    // Delete both temporary contexts while preserving the first failure.
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
        // Empty locators use row number zero. Readers skip fragment access when
        // the fragment count is zero.
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

/// Copy an already loaded sequence into fragment rows.
/// Uses insert_sequence_streamed() for shard selection, row ordering and
/// source-version creation.
///
/// @tparam p_length_type Unsigned type used for sequence lengths.
/// @tparam p_bucket_minimum_length_bits Smallest bucket exponent.
/// @tparam p_bucket_maximum_length_bits Largest bucket exponent.
/// @param user_relid Parent table.
/// @param main_storage_id Parent storage generation.
/// @param db_oid Parent database.
/// @param spc_oid Parent tablespace.
/// @param main_relfilenode Parent table file number.
/// @param column_attno One-based sequence column number.
/// @param parent_tupdesc Parent column descriptions.
/// @param original_hdr Complete packed sequence. Must be non-null and not a locator.
/// @param current_cap_bytes Shard size limit. Zero gives each sequence its own
/// shard without a size limit.
/// @param cache Write cache kept alive by the caller.
/// @return Token holding the locator and unpublished source version.
/// @throws An error for invalid input or a failed fragment write.
template<class p_length_type,
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

    // Copy slices from the existing packed payload.
    const std::uint8_t* const src_data =
        reinterpret_cast<const std::uint8_t*>(original_hdr->data);
    const std::size_t src_bytes =
        static_cast<std::size_t>(bioseq::postgres::support::varlena_4byte_size(original_hdr)) - bioseq::pg_types::seq::header_bytes();

    // Fill one fragment without decoding or repacking its symbols.
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

// This writer accepts chunks before the final sequence length is known.
// It opens a shard on the first fragment and checks the size limit as rows
// are written. All fragments stay in that shard. If a write fails, PostgreSQL
// rollback removes the rows already stored.
/// Start a sequence write and prepare its fragment buffer.
/// The shard opens when the first fragment is written.
/// @param user_relid Parent table.
/// @param db_oid Parent database.
/// @param spc_oid Parent tablespace.
/// @param main_relfilenode Parent table file number.
/// @param main_storage_id Parent storage generation.
/// @param seq_attno One-based sequence column number.
/// @param parent_td Parent column descriptions, kept alive throughout the write.
/// @param alphabet_code Sequence alphabet code.
/// @param byte_width Packed storage-unit width in bytes.
/// @param cap_bytes Sequence size limit in bytes, or zero for no limit.
/// @param caches Write cache kept alive throughout the write.
/// @throws A translated PostgreSQL error for an invalid cache, parent or encoding.
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

/// Open a shard in the largest length bucket and lock it against other writers.
/// Repeated calls reuse the opened shard. The caller's cache keeps its relation
/// reference and the transaction owns its column writer.
void sequence_stream_writer::open_shard() {
    if (shard_ != nullptr) return;
    // Use the largest length bucket because the final length is still unknown.
    // Readers find the fragments through the bucket recorded in the locator.
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
    // Hold the shard lock until transaction end so fragments from another
    // writer cannot interrupt this sequence's row range.
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

/// Store one fragment and extend the sequence's consecutive row range.
/// @param payload Packed bytes to copy.
/// @param payload_bytes Bytes to copy, at most the fragment payload capacity.
/// @param bp Number of symbols in this fragment.
/// @param pad Unused positions in its final storage unit.
/// @throws A translated PostgreSQL error if the size limit, checksum or row
/// ordering check fails.
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
    // Convert allocation errors to C++ exceptions so PostgreSQL cannot jump
    // past the caller's C++ cleanup. write_column_row protects its own calls.
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

/// Accept packed bytes and write fragments that are known to be complete.
/// Keep the last fragment until finish() supplies its length and padding.
/// @param bytes Input copied into the buffer. Submit bounded chunks to limit
/// temporary memory, since the whole span is copied before fragments are written.
/// @param bp_count Unused. finish() supplies the sequence length.
/// @throws A translated PostgreSQL error if begin() was not called or a write fails.
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
    // Keep one full fragment until finish(): it may contain padding that only
    // the caller can identify at the sequence end. Earlier fragments have no padding.
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

/// Write the final fragment and record the unpublished sequence version.
/// @param total_bp Complete sequence length in symbols.
/// @param last_pad Unused positions in the final storage unit.
/// @return Token used to publish the locator after storing the parent row.
/// @throws A translated PostgreSQL error if begin() was not called or a write fails.
pending_sequence_source sequence_stream_writer::finish(
    sequence_size_t total_bp, std::uint16_t last_pad)
{
    if (!active_) {
        storage_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "bioseq.load: finish() before begin()");
    }
    // The remaining buffer holds the final fragment. Its length excludes
    // symbols already written in earlier fragments.
    const sequence_size_t emitted_bp =
        static_cast<sequence_size_t>(num_fragments_) * frag_bp_;
    if (!buffer_.empty()) {
        const sequence_size_t last_bp = total_bp - emitted_bp;
        write_fragment(buffer_.data(), buffer_.size(), last_bp, last_pad);
        buffer_.clear();
    }

    if (num_fragments_ == 0) {
        // Empty sequences have no fragment rows. Readers ignore this row number.
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

/// Clear buffered input and mark the writer inactive.
/// PostgreSQL transaction rollback must remove any fragment rows already written.
void sequence_stream_writer::abort() {
    // Clear local state so the writer can start another sequence.
    buffer_.clear();
    active_ = false;
    source_epoch_ = 0;
}



}  // namespace bioseq::storage
