/**
 * @file storage_adapter_conformance.cppm
 * @brief Compile-time layout conformance gates and parallel worker entry point.
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
export module bioseq.postgres.storage.indexing.chunked_storage:storage_adapter_conformance;
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
export import :table_storage_iteration;
export namespace bioseq::storage::indexing {

// ---- conformance gates ---------------------------------------------------
/**
 * @brief Detects accidental public exposure of segment-spill lifecycle state.
 * @tparam p_storage Storage adapter type examined by the compile-time gate.
 */
template <class p_storage>
concept publicly_mutable_minimizer_sequence_segment_spill_state = requires(p_storage& storage) {
    storage.segment_sorted_runs_;
    storage.segment_spill_binding_;
    storage.segment_spill_pg_poisoned_;
    storage.next_segment_spill_generation_token_;
};
static_assert(!publicly_mutable_minimizer_sequence_segment_spill_state<fragment_index_storage>);
static_assert(!std::is_move_assignable_v<fragment_index_storage>);
static_assert(!std::is_move_assignable_v<sequence_index_storage>);
static_assert(!std::is_move_assignable_v<table_index_storage>);
static_assert(
    bioseq::indexing::is_index_chunked_storage_for<fragment_index_storage,
                                                   sequence_index_layout::fragment_chunks>,
    "fragment_index_storage must satisfy CHUNKED_PER_FRAGMENT");
static_assert(
    bioseq::indexing::is_index_chunked_storage_for<sequence_index_storage,
                                                   sequence_index_layout::sequence_inline>,
    "sequence_index_storage must satisfy INLINE_PER_SEQUENCE");
static_assert(bioseq::indexing::is_index_chunked_storage_for<table_index_storage,
                                                             sequence_index_layout::table_chunks>,
              "table_index_storage must satisfy CHUNKED_PER_TABLE");

} // namespace bioseq::storage::indexing
/**
 * @brief Runs the dynamic-shared-memory hash-major parallel scan probe worker.
 * @param[in] segment Attached PostgreSQL dynamic shared-memory segment.
 * @param[in] toc Shared table of contents containing the validated probe state.
 */
extern "C" PGDLLEXPORT void bioseq_hash_major_parallel_scan_probe_worker_main(dsm_segment* segment,
                                                                              shm_toc* toc) {
    bioseq::storage::indexing::hash_major_minimizer_parallel_scan_probe_worker_main_pg(segment,
                                                                                       toc);
}
