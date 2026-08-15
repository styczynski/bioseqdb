/**
 * @file table_storage_iteration.cppm
 * @brief Table-layout sequence traversal under PostgreSQL snapshot ownership.
 */
module;
#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>

extern "C" {
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include "common/relpath.h"
#include "miscadmin.h"
#include "port.h"
#include "port/atomics.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
export module bioseq.postgres.storage.indexing.chunked_storage:table_storage_iteration;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // the concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;        // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // canonical bounded base decoding
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.instrumentation.clock;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import :catalog;
import :fragment_image;
import :generation_writer;
import :generation_session;
import :hash_major_writer;
import :hash_major_session;
export import :storage_adapter_contract;
export import :fragment_storage_spill;
export namespace bioseq::storage::indexing {

// ---- table_index_storage::for_each_sequence (test-only CHUNKED_PER_TABLE) ---
template <class p_callback>
inline void table_index_storage::for_each_sequence(p_callback&& loader) {
    Relation main = table_open(user_relid_, AccessShareLock);
    TupleDesc td = RelationGetDescr(main);
    Snapshot snap = RegisterSnapshot(GetTransactionSnapshot());
    const std::size_t total = static_cast<std::size_t>(column_table_row_count(main));
    List* projected = list_make1_int(column_attno_);
    column_read_state* rs = begin_column_read(
        main, td, projected, NIL, own_mctx_, snap, /*randomAccess=*/false, nullptr);
    MemoryContext inner =
        AllocSetContextCreate(own_mctx_, "seqidx_table_inner_seq", ALLOCSET_DEFAULT_SIZES);
    std::vector<Datum> vals(td->natts);
    std::vector<bool> nullsv(td->natts);
    Datum* valp = vals.data();
    std::vector<char> nullbuf(td->natts);
    bool* nulls = reinterpret_cast<bool*>(nullbuf.data());
    std::uint64_t rowno = 0;
    while (read_next_column_row(rs, valp, nulls, &rowno)) {
        if (nulls[column_attno_ - 1])
            continue;
        MemoryContextReset(inner);
        MemoryContext previous_context = MemoryContextSwitchTo(inner);
        auto hdr_detoasted = bioseq::postgres::support::detoast_as<bioseq::pg_types::seq>(
            bioseq::postgres::support::pg_datum::from_raw(valp[column_attno_ - 1]));
        bioseq::pg_types::seq* hdr = hdr_detoasted.ptr;
        MemoryContextSwitchTo(previous_context);
        const std::uint8_t alpha = hdr->alphabet_code;
        const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
        if (!is_locator_stub(hdr)) {
            bioseq::pg_types::seq::dispatch_instance(
                alpha, bw, [hdr, &loader, rowno, total]<class p_sequence_type>() {
                    compact_sequence_range<p_sequence_type> rng{*hdr};
                    const auto view = bioseq::storage::make_lazy_sequence_view(rng);
                    loader(view, static_cast<std::size_t>(rowno), total);
                });
        } else {
            const sequence_locator loc = decode_locator_from_stub(hdr);
            bioseq::pg_types::seq::dispatch_instance(
                alpha, bw, [this, &loader, loc, rowno, total, rs, inner]<class p_sequence_type>() {
                    using source_binding =
                        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_type>;
                    const source_binding bound_source{
                        db_oid_,
                        tablespace_oid_,
                        main_relfilenode_,
                        column_attno_,
                        loc,
                        source_binding::expected_last_fragment_padding(loc),
                        rs->snapshot,
                        read_caches_,
                        inner};
                    bioseq::storage::with_postgres_lazy_sequence<p_sequence_type>(
                        bound_source, [&loader, rowno, total](auto view) {
                            loader(view, static_cast<std::size_t>(rowno), total);
                        });
                });
        }
        hdr_detoasted.free_if_copy();
    }
    // with_postgres_lazy_sequence keyed cached fragment readers by rs->snapshot.
    // Release them before the outer reader unregisters a refreshed snapshot.
    if (read_caches_ != nullptr) {
        read_caches_->close_all(/*commit=*/false);
    }
    end_column_read(rs);
    UnregisterSnapshot(snap);
    MemoryContextDelete(inner);
    table_close(main, AccessShareLock);
}

} // namespace bioseq::storage::indexing
