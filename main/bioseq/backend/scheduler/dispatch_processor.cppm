/*-------------------------------------------------------------------------
 *
 * scheduler/dispatch_processor.cppm
 *
 * Heavy minimap/pipe implementation for BatchProcessor. Kept out of the
 * dispatch_buffer interface so edits to tuple-buffer or admin code do not
 * force that translation unit to reinstantiate the full supported alphabet x
 * storage-width minimap pipeline.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/highway.h>
#include "common.h"
#include "columnar/columnar.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

export module bioseq_scheduler.dispatch_processor;

import mlogging;
import pg_utils;
import bioseq_indexing.type;
import bioseq_indexing.traits;
import bioseq_indexing.minimizer_hashmap;
import bioseq_indexing.minimizer_sequence;
import bioseq_storage_indexing_pg;
import bioseq_storage_indexing_handles;
import bioseq_storage_indexing_residency_pg;
import bioseq_storage_indexing_sorted_runs_pg;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_sequences.minimap_port;
import bioseq_sequences.pipe;
import bioseq_types.pg_compact_sequence;
import bioseq_instr;
import bioseq_storage;
import bioseq_storage_lazy;
import bioseq_storage_scanner;
import bioseq_storage_scanner_pg;
import bioseq_parallelism.mode;
import bioseq_parallelism.concepts;
import bioseq_parallelism.pool;
import bioseq_parallelism.memory_admission;
import bioseq_scheduler.supervisor_pg;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

export constexpr size_t kDispatchBatchCapacity = 1000;
constexpr std::string_view kMinimizersPerFragmentIndex =
    bioseq::indexing::MinimizerHashmapFragmentIndex::index_name;
constexpr std::string_view kMinimizersPerSequenceIndex =
    bioseq::indexing::index_descriptor<
        bioseq::indexing::MinimizerSequenceMergedIndex>::index_name;

/*
 * BatchProcessor drives minimap_port across a batch of (query, target)
 * pairs that all share the same (alphabet_code, storage_unit_byte_width) typmod. The
 * caller hands us the already-detoasted query varlena (captured at
 * BeginCustomScan — see columnar_customscan.cppm) and an array of per-row
 * target varlenas (captured at submit_scan — see TupleDispatchBuffer).
 *
 * Dispatch: we branch on (alphabet_code, storage_unit_byte_width) exactly once, then
 * inside the matching arm all eight compact_sequence_range<Dst>
 * specialisations compile down to their own minimap_port instantiation.
 * No std::variant, no runtime type erasure on the hot path.
 */
/*
 * Execution counters captured while BatchProcessor walks a batch. Used by
 * EXPLAIN to print an accurate "Recomputed minimizers: X%" estimate.
 * The struct lives next to the BatchProcessor instance inside the
 * TupleDispatchBuffer so EXPLAIN can read it after run_batch_and_build_cursors
 * without doing a second walk.
 */
export struct BatchExecutionStats {
    /// Total rows submitted to process_sequences().
    size_t targets_considered = 0;
    /// Rows that arrived with a prebuilt index pointer and reused it.
    size_t targets_with_prebuilt_index = 0;
    /// Rows whose minimizers had to be recomputed from the compact_sequence_range.
    size_t targets_recomputed_minimizers = 0;
    /// Effective parameters used for the last batch. Captured for EXPLAIN so
    /// the user sees the actually-applied values after overrides + size-heuristic
    /// adjustments.
    Minimap2Params last_effective_params{};

    [[nodiscard]] float recomputed_fraction() const {
        return targets_considered == 0
            ? 0.0f
            : static_cast<float>(targets_recomputed_minimizers)
              / static_cast<float>(targets_considered);
    }
};

export class BatchProcessor {
public:
    /*
     * DOC0264 / DOC0274 — context required to wire scanner_pg into the
     * per-row pipeline dispatch. When non-null, the dispatch loop uses
     * `sequence_scanner_pg<S>` (PG-backed for stub-backed targets, in-memory
     * for the query literal). When null, the dispatch loop falls back to
     * the Phase B baseline `with_lazy_in_memory`.
     */
    struct ScannerPgContext {
        Oid                                              db_oid               = InvalidOid;
        Oid                                              tablespace_oid       = InvalidOid;
        Oid                                              main_relfilenode     = InvalidOid;
        std::int16_t                                     mmsearch_seq_attno   = 0;
        bioseq::storage::seq_caches*                     caches               = nullptr;
        MemoryContext                                    parent_mctx          = nullptr;
        // Per-row optional locator. nullopt means "use the inline pgseq_t at
        // target_hdrs[i]" — scanner_pg is then constructed in in-memory mode.
        std::array<std::optional<bioseq::storage::seq_locator>,
            kDispatchBatchCapacity>                       target_locators{};
        // DOC0279 §11.3c — per-row columnar rowno (== index query_key) and the
        // user relation oid, so the stored per-fragment index can be assembled.
        std::array<std::int64_t, kDispatchBatchCapacity>  target_rownos{};
        Oid                                               user_relid           = InvalidOid;
        bool                                              read_index_active    = false;
        // DOC0279 §11.3/§12.3 — the table's ACTIVE read index for the target
        // column (resolve_active_read_index), default minimizers_per_fragment.
        std::string                                       active_index;
    };

    /*
     * Run the minimap2 pipeline across a batch. DOC0279 §11.3 — per target
     * row, the loop ASSEMBLES the table's active per-sequence minimizer index
     * from its chunked storage (gated by read_index_active + per-row presence)
     * or RECOMPUTES it; the two are mixed freely within one batch.
     *
     * DOC0264 / DOC0274 — production dispatch posture.
     *
     * The Phase C `sequence_scanner_pg<S>` (in module
     * `bioseq_storage_scanner_pg`) is now wired into the dispatch loop.
     * `scanner_pg_ctx` carries the per-batch identity (db/spc/relfilenode/
     * attno/seq_caches/parent_mctx) plus the per-row optional locator the
     * caller decoded at submit_scan time. When `scanner_pg_ctx` is non-null
     * AND the user has not forced the in-memory baseline via
     * `bioseq.test_force_inmemory_scanner = on`, the loop dispatches:
     *   - target through `sequence_scanner_pg<S>` (PG-backed when locator
     *     present; in-memory wrap of the inline pgseq_t otherwise);
     *   - query through `sequence_scanner_pg<S>` in-memory wrap of the
     *     batch's query literal.
     * Both share the same `Scanner = sequence_scanner_pg<S>` template, so
     * the pipeline tree adds one instantiation (alongside the existing
     * `sequence_scanner_none` and `sequence_scanner_test_chunked` trees).
     *
     * NOTE on parameter passing: `target_hdrs` and `query_ids` are full-size
     * `kDispatchBatchCapacity`-element arrays passed by const-ref so the call
     * does not copy them. The result map is RVO'd / moved on return.
     */
    /*
     * Run the minimap2 pipeline across the fixed dispatch-buffer batch shape.
     * The implementation lives in dispatch_processor.cppm so this module
     * interface does not instantiate the full pgseq_supported_alphabets ×
     * storage-width minimap pipeline whenever tuple-buffer/admin code changes.
     */
    [[nodiscard]] std::map<int32_t, std::vector<MatchResult>> process_sequences(
        size_t batch_size,
        uint8_t alphabet_code,
        uint8_t storage_unit_byte_width,
        const pgseq_t* query_hdr,
        const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
        const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
        const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
        const Minimap2Params& base_params,
        BatchExecutionStats& stats,
        ScannerPgContext* scanner_pg_ctx = nullptr);

};

enum class DeferredMinimizerIndexKind : std::uint8_t {
    per_fragment,
    per_sequence,
};

struct DeferredMinimizerPersistChoice {
    DeferredMinimizerIndexKind kind = DeferredMinimizerIndexKind::per_fragment;
    std::string_view index_name = kMinimizersPerFragmentIndex;
};

[[nodiscard]] DeferredMinimizerIndexKind deferred_kind_for_index(
        std::string_view index_name) noexcept {
    return index_name == kMinimizersPerSequenceIndex
        ? DeferredMinimizerIndexKind::per_sequence
        : DeferredMinimizerIndexKind::per_fragment;
}

[[nodiscard]] std::optional<DeferredMinimizerPersistChoice>
choose_deferred_minimizer_persist_index(
        const BatchProcessor::ScannerPgContext& ctx) {
    if (!OidIsValid(ctx.user_relid) || ctx.mmsearch_seq_attno <= 0) {
        return std::nullopt;
    }

    constexpr auto minimizers = bioseq::indexing::MINIMIZER_INDICES;
    std::array<bool, minimizers.size()> enabled{};
    std::array<bool, minimizers.size()> update_on_insert{};
    for (std::size_t i = 0; i < minimizers.size(); ++i) {
        enabled[i] = true;
        update_on_insert[i] =
            bioseq::indexing::default_update_on_insert_contains(minimizers[i]);
    }

    if (SPI_connect() == SPI_OK_CONNECT) {
        const std::string sql = fmt::format(
            "SELECT index_name, enabled, update_on_insert "
            "FROM bioseq_internal.index_policy "
            "WHERE user_relid={}::oid AND main_relfilenode={}::oid "
            "AND column_attno={}",
            static_cast<unsigned>(ctx.user_relid),
            static_cast<unsigned>(ctx.main_relfilenode),
            static_cast<int>(ctx.mmsearch_seq_attno));
        if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT) {
            for (std::uint64_t row = 0; row < SPI_processed; ++row) {
                char* nm = SPI_getvalue(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 1);
                if (nm == nullptr) {
                    continue;
                }
                bool isnull = false;
                const bool row_enabled = DatumGetBool(SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 2, &isnull));
                const bool row_update = DatumGetBool(SPI_getbinval(
                    SPI_tuptable->vals[row], SPI_tuptable->tupdesc, 3, &isnull));
                for (std::size_t i = 0; i < minimizers.size(); ++i) {
                    if (minimizers[i] == std::string_view{nm}) {
                        enabled[i] = row_enabled;
                        update_on_insert[i] = row_update;
                        break;
                    }
                }
            }
        }
        SPI_finish();
    }

    for (std::size_t i = 0; i < minimizers.size(); ++i) {
        if (enabled[i] && update_on_insert[i]) {
            return DeferredMinimizerPersistChoice{
                .kind = deferred_kind_for_index(minimizers[i]),
                .index_name = minimizers[i],
            };
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string make_async_persist_temp_path(
        std::string_view index_name,
        std::uint64_t sequence_rowno) {
    const char* prefix =
        index_name == kMinimizersPerSequenceIndex ? "mps" : "mpf";
    return fmt::format(
        "/tmp/bioseq_async_{}_{}_{}_{}.bin",
        prefix,
        static_cast<long long>(::getpid()),
        static_cast<unsigned long long>(sequence_rowno),
        static_cast<unsigned long long>(bioseq::instr::clock::now_ns()));
}

/*
 * Scale k/w for the small-query regime typical inside Postgres — short
 * motifs vs per-row sequences under 1 kb. Runs once per row before the
 * core pipeline. Factored out of the hot lambda so all code paths
 * (recompute vs prebuilt index) share a single size-heuristic knob and
 * so EXPLAIN can replay the same derivation for its param printout.
 *
 * The heuristic only fires when the user left the fields at their
 * minimap2 defaults. Any aligner_options() override or non-default GUC
 * value short-circuits the adjustment so we never silently overwrite a
 * user-supplied tuning value.
 */
[[nodiscard]] inline Minimap2Params apply_size_heuristic(
    const Minimap2Params& base, size_t min_bp)
{
    /*
     * Only scale when every relevant parameter still matches the minimap2
     * default. If the caller has already tuned one of these fields, we
     * trust them and leave the batch alone.
     */
    const bool all_defaults =
        base.kmer_size       == 15 &&
        base.window_size     == 10 &&
        base.min_chain_score == 40 &&
        base.min_seed_count  == 3;
    if (!all_defaults) {
        return base;
    }
    Minimap2Params scaled = base;
    if (min_bp < 50) {
        scaled.kmer_size       = 8;
        scaled.window_size     = 3;
        scaled.min_chain_score = 10;
        scaled.min_seed_count  = 1;
    } else if (min_bp < 200) {
        scaled.kmer_size       = 10;
        scaled.window_size     = 5;
        scaled.min_chain_score = 20;
        scaled.min_seed_count  = 2;
    }
    return scaled;
}

struct OpportunisticIndexChunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::vector<char> bytes{};
};

struct OpportunisticFragmentIndexStorage {
    std::map<bioseq::indexing::index_sub_key_t, std::vector<std::size_t>> plans{};
    std::vector<OpportunisticIndexChunk> chunks{};

    [[noreturn]] void index_error(std::string_view msg) const {
        pg_log(ERROR, "bioseq opportunistic index: {}", msg);
        abort();
    }

    [[nodiscard]] bioseq::indexing::MinimizerHashmapFragmentIndex::SerializedHeader
    validate_minimizer_blob_or_error(
            const char* p,
            std::size_t bytes,
            std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using H = bioseq::indexing::MinimizerHashmapFragmentIndex::SerializedHeader;
        using B = bioseq::indexing::MinimizerHashmapFragmentIndex::SerializedBucket;
        if (bytes < sizeof(H)) {
            index_error("minimizer blob truncated header");
        }
        H h{};
        std::memcpy(&h, p, sizeof(H));
        if (h.magic != bioseq::indexing::MinimizerHashmapFragmentIndex::kSerializeMagic
            || h.version != bioseq::indexing::MinimizerHashmapFragmentIndex::kSerializeVersion) {
            index_error("minimizer blob magic/version");
        }
        const std::size_t expect =
            sizeof(H) + sizeof(B) * h.bucket_count + sizeof(std::uint64_t) * h.total_positions;
        if (bytes != expect) {
            index_error("minimizer blob size mismatch");
        }
        if (expected_bucket_count && h.bucket_count != *expected_bucket_count) {
            index_error("minimizer blob bucket_count mismatch");
        }
        return h;
    }

    void validate_minimizer_directory_or_error(const char*, std::size_t bytes) const {
        if (bytes < sizeof(std::int64_t)) {
            index_error("directory truncated");
        }
    }

    void validate_minimizer_chunk_metadata_or_error(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no,
            std::size_t min_bytes,
            std::size_t) const {
        if (chunk_byte_size(sub_key, chunk_no) < min_bytes) {
            index_error("chunk metadata below minimum size");
        }
    }

    [[nodiscard]] std::size_t number_of_chunks(
            bioseq::indexing::index_sub_key_t sub_key) const {
        std::size_t count = 0;
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t count = 0;
        bioseq::indexing::index_sub_key_t last =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have = false;
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (!have || chunk.sub_key != last) {
                ++count;
                last = chunk.sub_key;
                have = true;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t chunk_byte_size(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no) const {
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk.bytes.size();
            }
        }
        return 0;
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no) {
        for (OpportunisticIndexChunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return reinterpret_cast<const T*>(chunk.bytes.data());
            }
        }
        index_error("chunk missing");
    }

    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n =
            bytes_count == 0
                ? 1
                : (bytes_count + bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES - 1)
                    / bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
        std::vector<std::size_t> plan(n);
        std::size_t remaining = bytes_count;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t bytes =
                std::min(remaining, bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES);
            plan[i] = bytes;
            remaining -= bytes;
        }
        plans[sub_key] = std::move(plan);
    }

    void resize_chunks(
            bioseq::indexing::index_sub_key_t sub_key,
            std::span<const std::size_t> per_chunk_bytes) {
        plans[sub_key] = std::vector<std::size_t>(
            per_chunk_bytes.begin(), per_chunk_bytes.end());
    }

    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    template <class T, class F>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, F&& writer) {
        auto it = plans.find(sub_key);
        if (it == plans.end()) {
            index_error("write_to_each without resize");
        }
        const std::vector<std::size_t>& plan = it->second;
        for (std::size_t chunk_no = 0; chunk_no < plan.size();) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (payload_bytes % sizeof(T) != 0) {
                index_error("chunk payload not a whole number of T");
            }
            OpportunisticIndexChunk chunk{};
            chunk.sub_key = sub_key;
            chunk.chunk_no = static_cast<std::uint32_t>(chunk_no);
            chunk.bytes.assign(payload_bytes, char{0});
            const std::size_t next =
                writer(chunk_no, reinterpret_cast<T*>(chunk.bytes.data()),
                       payload_bytes / sizeof(T));
            if (next < chunk_no) {
                index_error("writer moved backwards");
            }
            chunks.push_back(std::move(chunk));
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        plans.erase(it);
    }

    template <class F>
    void read_chunks_in_range(
            bioseq::indexing::index_sub_key_t lo,
            bioseq::indexing::index_sub_key_t hi,
            F&& cb) {
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (chunk.sub_key >= lo && chunk.sub_key < hi) {
                cb(chunk.sub_key, chunk.bytes.data(), chunk.bytes.size());
            }
        }
    }

    void for_each_sub_key(auto&& fn) const {
        bioseq::indexing::index_sub_key_t last =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have = false;
        for (const OpportunisticIndexChunk& chunk : chunks) {
            if (!have || chunk.sub_key != last) {
                fn(chunk.sub_key);
                last = chunk.sub_key;
                have = true;
            }
        }
    }

    void clear_entity() {
        plans.clear();
        chunks.clear();
    }

    void clear(std::string_view) {
        clear_entity();
    }

    void discard_sub_keys(
            bioseq::indexing::index_sub_key_t lo,
            bioseq::indexing::index_sub_key_t hi) {
        chunks.erase(
            std::remove_if(
                chunks.begin(), chunks.end(),
                [lo, hi](const OpportunisticIndexChunk& chunk) {
                    return chunk.sub_key >= lo && chunk.sub_key <= hi;
                }),
            chunks.end());
    }

    [[nodiscard]] std::size_t spill_sorted_run(
            std::span<const std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned(
            std::vector<std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned_admitted(
            std::vector<std::pair<std::uint64_t, std::uint64_t>>,
            std::uint64_t,
            bioseq::parallelism::MemoryPermit permit) {
        permit.release();
        index_error("external sort spill is not supported by opportunistic storage");
    }

    void merge_runs(auto&&) {
        index_error("external sort merge is not supported by opportunistic storage");
    }

    void release_resident_runs_noexcept() noexcept {}

    void release_runs_noexcept() noexcept {}

    void release_runs() {}

    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return 0;
    }
};

struct AsyncPersistFileChunkHeader {
    std::int64_t sub_key;
    std::uint32_t chunk_no;
    std::uint32_t reserved;
    std::uint64_t size;
};

static_assert(sizeof(AsyncPersistFileChunkHeader) == 24);

struct AsyncPersistFileFragmentStorage {
    std::map<bioseq::indexing::index_sub_key_t, std::vector<std::size_t>> plans{};
    std::string path{};
    std::ofstream out{};
    std::uint64_t chunk_count = 0;
    std::uint64_t file_bytes = 0;
    Oid db_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::uint64_t sequence_rowno = 0;
    bioseq::storage::indexing::SortedPairRunStore sorted_runs{};

    explicit AsyncPersistFileFragmentStorage(std::string p)
        : path(std::move(p)),
          out(path, std::ios::binary | std::ios::trunc) {
        if (!out) {
            pg_log(ERROR,
                   "bioseq async persist: failed to open temp payload file {}",
                   path);
        }
    }

    [[noreturn]] void index_error(std::string_view msg) const {
        pg_log(ERROR, "bioseq async persist file storage: {}", msg);
        abort();
    }

    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n =
            bytes_count == 0
                ? 1
                : (bytes_count + bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES - 1)
                    / bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
        std::vector<std::size_t> plan(n);
        std::size_t remaining = bytes_count;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t bytes =
                std::min(remaining, bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES);
            plan[i] = bytes;
            remaining -= bytes;
        }
        plans[sub_key] = std::move(plan);
    }

    void resize_chunks(
            bioseq::indexing::index_sub_key_t sub_key,
            std::span<const std::size_t> per_chunk_bytes) {
        std::vector<std::size_t> plan(per_chunk_bytes.begin(), per_chunk_bytes.end());
        for (std::size_t bytes : plan) {
            if (bytes > bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES) {
                index_error("chunk payload exceeds MAX_INDEX_CHUNK_SIZE_BYTES");
            }
        }
        plans[sub_key] = std::move(plan);
    }

    [[nodiscard]] std::size_t number_of_sub_keys() const {
        return plans.size();
    }

    [[nodiscard]] std::size_t number_of_chunks(
            bioseq::indexing::index_sub_key_t sub_key) const {
        if (auto it = plans.find(sub_key); it != plans.end()) {
            return it->second.size();
        }
        return 0;
    }

    [[nodiscard]] std::size_t chunk_byte_size(
            bioseq::indexing::index_sub_key_t sub_key,
            std::size_t chunk_no) const {
        if (auto it = plans.find(sub_key);
            it != plans.end() && chunk_no < it->second.size()) {
            return it->second[chunk_no];
        }
        return 0;
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(
            bioseq::indexing::index_sub_key_t,
            std::size_t) {
        index_error("read_chunk is not supported by async write-only storage");
    }

    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    template <class F>
    void read_chunks_in_range(
            bioseq::indexing::index_sub_key_t,
            bioseq::indexing::index_sub_key_t,
            F&&) {
        index_error("read_chunks_in_range is not supported by async write-only storage");
    }

    template <class F>
    void for_each_sub_key(F&& fn) const {
        for (const auto& [sub_key, _] : plans) {
            fn(sub_key);
        }
    }

    void clear_entity() {
        plans.clear();
    }

    void clear(std::string_view) {
        clear_entity();
    }

    template <class T, class F>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, F&& writer) {
        auto it = plans.find(sub_key);
        if (it == plans.end()) {
            index_error("write_to_each without resize");
        }
        const std::vector<std::size_t>& plan = it->second;
        for (std::size_t chunk_no = 0; chunk_no < plan.size();) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (payload_bytes % sizeof(T) != 0) {
                index_error("chunk payload not a whole number of T");
            }
            std::vector<char> payload(payload_bytes, char{0});
            const std::size_t next =
                writer(chunk_no, reinterpret_cast<T*>(payload.data()),
                       payload_bytes / sizeof(T));
            if (next < chunk_no || next > plan.size()) {
                index_error("writer produced invalid next chunk");
            }

            const AsyncPersistFileChunkHeader header{
                .sub_key = static_cast<std::int64_t>(sub_key),
                .chunk_no = static_cast<std::uint32_t>(chunk_no),
                .reserved = 0,
                .size = static_cast<std::uint64_t>(payload_bytes),
            };
            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            if (!payload.empty()) {
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }
            if (!out) {
                index_error("write failed");
            }
            ++chunk_count;
            file_bytes += sizeof(header) + payload_bytes;
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        plans.erase(it);
    }

    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        if (bioseq_index_run_buffer_bytes > 0) {
            return static_cast<std::size_t>(bioseq_index_run_buffer_bytes);
        }
        constexpr std::uint64_t desired = std::uint64_t{512} << 20;
        constexpr std::uint64_t floor = std::uint64_t{64} << 20;
        const std::uint64_t cap =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .index_memory_cap_bytes();
        if (cap == 0) {
            return static_cast<std::size_t>(desired);
        }
        return static_cast<std::size_t>(
            std::max<std::uint64_t>(std::min(desired, cap), floor));
    }

    [[nodiscard]] std::size_t spill_sorted_run(
            std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted) {
        return sorted_runs.spill_sorted_run(sorted);
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned(
            std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted) {
        return sorted_runs.spill_sorted_run_owned(std::move(sorted));
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned_admitted(
            std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted,
            std::uint64_t admitted_bytes,
            bioseq::parallelism::MemoryPermit permit) {
        return sorted_runs.spill_sorted_run_owned_admitted(
            std::move(sorted), admitted_bytes, std::move(permit));
    }

    template <class F>
    void merge_runs(F&& on_kmer) {
        sorted_runs.merge_runs(std::forward<F>(on_kmer));
    }

    void release_resident_runs_noexcept() noexcept {
        sorted_runs.release_resident_runs_noexcept();
    }

    void release_runs_noexcept() noexcept {
        sorted_runs.release_runs_noexcept();
    }

    void release_runs() {
        release_runs_noexcept();
    }

    [[nodiscard]] bioseq::storage::indexing::SortedRunStoreStats
    sorted_run_stats() const noexcept {
        return sorted_runs.stats();
    }

    void snapshot_sorted_run_stats_before_release() {
        const auto st = sorted_runs.stats();
        const std::uint64_t total_runs = sorted_runs.run_count();
        bioseq::storage::indexing::g_last_index_merge_run_count = total_runs;
        bioseq::storage::indexing::g_last_mps_build_stats =
            bioseq::storage::indexing::LastMpsBuildStats{
                .backend_pid = static_cast<std::uint64_t>(MyProcPid),
                .db_oid = static_cast<std::uint64_t>(db_oid),
                .relfilenode = static_cast<std::uint64_t>(main_relfilenode),
                .sequence_rowno = sequence_rowno,
                .total_run_count = total_runs,
                .resident_runs = st.resident_runs,
                .file_runs = st.file_runs,
                .resident_run_bytes = st.resident_bytes,
                .resident_peak_bytes = st.resident_peak_bytes,
                .spilled_run_bytes = st.spilled_bytes,
                .postgres_temp_file_bytes = st.postgres_temp_file_bytes,
                .postgres_temp_file_count = st.postgres_temp_file_count,
                .merge_reader_buffer_bytes = st.merge_reader_buffer_bytes,
                .merge_pass_count = st.merge_pass_count,
                .spill_count_from_pressure = st.spill_count_from_pressure,
                .resident_demotions = st.resident_demotions,
                .position_spill_bytes = st.position_spill_bytes,
                .position_spill_count = st.position_spill_count,
            };
        JustLog("[bioseq.index.mps_build.row] backend_pid={} async_file=1 db_oid={} relfilenode={} sequence_rowno={} "
                "resident_runs={} file_runs={} resident_run_bytes={} resident_peak_bytes={} "
                "spilled_run_bytes={} postgres_temp_file_bytes={} postgres_temp_file_count={} "
                "merge_reader_buffer_bytes={} merge_pass_count={} spill_count_from_pressure={} "
                "resident_demotions={} position_spill_bytes={} position_spill_count={}",
                MyProcPid,
                static_cast<unsigned long long>(db_oid),
                static_cast<unsigned long long>(main_relfilenode),
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(st.resident_runs),
                static_cast<unsigned long long>(st.file_runs),
                static_cast<unsigned long long>(st.resident_bytes),
                static_cast<unsigned long long>(st.resident_peak_bytes),
                static_cast<unsigned long long>(st.spilled_bytes),
                static_cast<unsigned long long>(st.postgres_temp_file_bytes),
                static_cast<unsigned long long>(st.postgres_temp_file_count),
                static_cast<unsigned long long>(st.merge_reader_buffer_bytes),
                static_cast<unsigned long long>(st.merge_pass_count),
                static_cast<unsigned long long>(st.spill_count_from_pressure),
                static_cast<unsigned long long>(st.resident_demotions),
                static_cast<unsigned long long>(st.position_spill_bytes),
                static_cast<unsigned long long>(st.position_spill_count));
    }

    void finish() {
        out.flush();
        if (!out) {
            index_error("flush failed");
        }
        out.close();
    }

    void remove_noexcept() noexcept {
        try {
            if (out.is_open()) {
                out.close();
            }
            if (!path.empty()) {
                std::remove(path.c_str());
            }
            release_runs_noexcept();
        } catch (...) {
        }
    }
};

[[nodiscard]] bioseq::parallelism::pg::PersistTarget
make_fragment_index_persist_target(
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        std::uint64_t sequence_rowno,
        std::string_view index_name = kMinimizersPerFragmentIndex) {
    return bioseq::parallelism::pg::PersistTarget{
        .db_oid = ctx.db_oid,
        .user_relid = ctx.user_relid,
        .tablespace_oid = ctx.tablespace_oid,
        .main_relfilenode = ctx.main_relfilenode,
        .column_attno = ctx.mmsearch_seq_attno,
        .bucket_idx = static_cast<std::uint8_t>(loc.bucket_idx),
        .shard_idx = loc.shard_idx,
        .sequence_rowno = sequence_rowno,
        .index_name = index_name,
    };
}

[[nodiscard]] bool finish_and_stage_async_index_file(
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        std::uint64_t sequence_rowno,
        std::string_view index_name,
        AsyncPersistFileFragmentStorage& file_store) {
    file_store.finish();
    const auto persist_target =
        make_fragment_index_persist_target(ctx, loc, sequence_rowno, index_name);
    return bioseq::parallelism::pg::stage_async_index_persist_file(
        persist_target,
        std::move(file_store.path),
        file_store.file_bytes,
        file_store.chunk_count);
}

template <class LazyQuery, class ResidentTarget>
[[nodiscard]] MatchResultCollection
run_recompute_with_deferred_minimizer_persist(
        LazyQuery& lazy_query,
        ResidentTarget& resident_target,
        const Minimap2Params& params,
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        std::uint64_t sequence_rowno,
        const DeferredMinimizerPersistChoice& choice) {
    std::string temp_path =
        make_async_persist_temp_path(choice.index_name, sequence_rowno);
    AsyncPersistFileFragmentStorage file_store{std::move(temp_path)};
    file_store.db_oid = ctx.db_oid;
    file_store.main_relfilenode = ctx.main_relfilenode;
    file_store.sequence_rowno = sequence_rowno;
    MatchResultCollection collection;
    bool staged = false;
    bool async_worker_spawned = false;
    PG_TRY();
    {
        if (choice.kind == DeferredMinimizerIndexKind::per_fragment) {
            collection = minimap_port_segmented(
                lazy_query, resident_target, params, &file_store);
            staged = finish_and_stage_async_index_file(
                ctx, loc, sequence_rowno, choice.index_name, file_store);
        } else {
            // DOC0301 iteration 2 — answer the query with the recompute FIRST,
            // then hand the per-sequence index build to a dynamic background
            // worker (off the foreground read backend). Spawning AFTER the
            // recompute is deliberate: the worker's CPU/RAM-heavy collect+sort
            // must not compete with the timed recompute (an earlier spawn inflated
            // the measured region from ~29 s to ~48 s). This collapses first-query
            // latency toward the per-fragment no-index target while the disk-frugal
            // build (iteration 1) runs in the worker during the post-query window.
            collection = minimap_port_segmented(
                lazy_query, resident_target, params);
            if (bioseq_async_mps_build_worker
                && OidIsValid(ctx.user_relid)
                && bioseq::parallelism::pg::spawn_mps_async_build_worker(
                       ctx.db_oid, ctx.user_relid)) {
                async_worker_spawned = true;
            } else {
                // Fallback (worker disabled / unregisterable / inline non-locator
                // row): iteration-1 disk-frugal FOREGROUND build + stage.
                bioseq::indexing::MinimizerSequenceMergedIndex<
                    AsyncPersistFileFragmentStorage>::build(
                        file_store, resident_target);
                staged = finish_and_stage_async_index_file(
                    ctx, loc, sequence_rowno, choice.index_name, file_store);
            }
        }
    }
    PG_CATCH();
    {
        file_store.remove_noexcept();
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (async_worker_spawned) {
        // No file payload was written; drop the unused temp file handle.
        file_store.remove_noexcept();
        staged = true;
    }

    JustLog("[bioseq] index: recompute async-persist row={} index={} target_bp={} chunks={} file_bytes={} async_staged={}",
            static_cast<unsigned long long>(sequence_rowno),
            choice.index_name,
            static_cast<unsigned long long>(loc.total_bp_size),
            static_cast<unsigned long long>(file_store.chunk_count),
            static_cast<unsigned long long>(file_store.file_bytes),
            staged);
    return collection;
}

template <class R>
struct ResidentTargetForWorkers {
    using traits = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;
    using resident_config = sequence_custom_alphabet<
        typename traits::alphabet_type,
        storage_unit,
        traits::bits_per_char>;
    using resident_range = compact_sequence_range<resident_config>;

    bioseq::parallelism::MemoryPermit permit;
    resident_range range;
};

template <class R, class Scanner>
[[nodiscard]] std::optional<ResidentTargetForWorkers<R>>
try_materialize_target_for_segment_workers(
        const bioseq::storage::lazy_compact_sequence<R, Scanner>& target_seq,
        std::uint64_t sequence_rowno) {
    if constexpr (!Scanner::supports_fragments()) {
        return std::nullopt;
    } else {
        using resident_t = ResidentTargetForWorkers<R>;
        using storage_unit = typename resident_t::storage_unit;

        const std::uint64_t total_units64 =
            static_cast<std::uint64_t>(target_seq.size());
        if (total_units64
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            pg_log(ERROR,
                   "bioseq resident target: target row {} has too many packed "
                   "units to materialize in one process",
                   static_cast<unsigned long long>(sequence_rowno));
        }
        const std::size_t total_units =
            static_cast<std::size_t>(total_units64);
        const std::uint64_t packed_bytes =
            bioseq::parallelism::saturating_mul(
                total_units64,
                static_cast<std::uint64_t>(sizeof(storage_unit)));
        constexpr std::uint64_t context_reserve_bytes = std::uint64_t{8} << 20;
        const std::uint64_t reserve_bytes =
            packed_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - context_reserve_bytes
                ? (std::numeric_limits<std::uint64_t>::max)()
                : packed_bytes + context_reserve_bytes;

        auto permit =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .try_reserve(reserve_bytes);
        if (!permit) {
            JustLog("[bioseq] resident target: row={} target_bp={} packed_bytes={} "
                    "not admitted; using scanner_pg serial fallback",
                    static_cast<unsigned long long>(sequence_rowno),
                    static_cast<unsigned long long>(target_seq.bp_size()),
                    static_cast<unsigned long long>(packed_bytes));
            return std::nullopt;
        }

        typename resident_t::resident_range::base_container packed(total_units);
        const std::size_t fragment_units = Scanner::fragment_storage_units;
        for (std::size_t fragment_no = 0;
             fragment_no < target_seq.number_of_fragments();
             ++fragment_no) {
            if ((fragment_no & std::size_t{0x3ff}) == 0) {
                CHECK_FOR_INTERRUPTS();
            }
            const std::span<const storage_unit> fragment =
                target_seq.scanner().read_fragment(fragment_no);
            const std::size_t dst_offset = fragment_no * fragment_units;
            if (dst_offset >= packed.size()) {
                target_seq.scanner().release_fragment(fragment_no);
                break;
            }
            const std::size_t copy_units =
                std::min(fragment.size(), packed.size() - dst_offset);
            std::copy_n(fragment.data(), copy_units, packed.data() + dst_offset);
            target_seq.scanner().release_fragment(fragment_no);
        }

        JustLog("[bioseq] resident target: row={} target_bp={} fragments={} "
                "packed_bytes={} materialized for segment workers",
                static_cast<unsigned long long>(sequence_rowno),
                static_cast<unsigned long long>(target_seq.bp_size()),
                target_seq.number_of_fragments(),
                static_cast<unsigned long long>(packed_bytes));

        return resident_t{
            std::move(*permit),
            resident_t::resident_range::from_packed(
                std::move(packed), target_seq.padding())};
    }
}

[[nodiscard]] bool stage_opportunistic_fragment_store_for_async_persist(
        const BatchProcessor::ScannerPgContext& ctx,
        const bioseq::storage::seq_locator& loc,
        const OpportunisticFragmentIndexStorage& ram_store,
        std::uint64_t sequence_rowno,
        std::string_view index_name = kMinimizersPerFragmentIndex) {
    if (!OidIsValid(ctx.user_relid) || ram_store.chunks.empty()) {
        return false;
    }

    std::vector<bioseq::parallelism::pg::PersistChunk> persist_chunks;
    persist_chunks.reserve(ram_store.chunks.size());
    for (const OpportunisticIndexChunk& chunk : ram_store.chunks) {
        persist_chunks.push_back(
            bioseq::parallelism::pg::PersistChunk{
                .sub_key = chunk.sub_key,
                .chunk_no = chunk.chunk_no,
                .bytes = std::span<const char>(
                    chunk.bytes.data(), chunk.bytes.size()),
            });
    }

    const bioseq::parallelism::pg::PersistTarget persist_target =
        make_fragment_index_persist_target(ctx, loc, sequence_rowno, index_name);
    return bioseq::parallelism::pg::stage_async_index_persist(
        persist_target,
        std::span<const bioseq::parallelism::pg::PersistChunk>(
            persist_chunks.data(), persist_chunks.size()));
}

[[nodiscard]] std::map<int32_t, std::vector<MatchResult>> BatchProcessor::process_sequences(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const pgseq_t* query_hdr,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>* per_row_query_hdrs,
    const std::array<const pgseq_t*, kDispatchBatchCapacity>& target_hdrs,
    const std::array<int32_t, kDispatchBatchCapacity>& query_ids,
    const Minimap2Params& base_params,
    BatchExecutionStats& stats,
    ScannerPgContext* scanner_pg_ctx)
{
    std::map<int32_t, std::vector<MatchResult>> results;
    const bool per_row_query_mode = per_row_query_hdrs != nullptr;

    if (!per_row_query_mode && query_hdr == nullptr) {
        JustLog("[bioseq] pg: BatchProcessor::process_sequences received null query_hdr; returning empty map");
        return results;
    }
    if (!per_row_query_mode
        && (query_hdr->alphabet_code != alphabet_code
            || query_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
        JustLog("[bioseq] pg: BatchProcessor::process_sequences query_hdr typmod ({},{}) disagrees with context ({},{}); refusing to dispatch",
                static_cast<unsigned>(query_hdr->alphabet_code),
                static_cast<unsigned>(query_hdr->storage_unit_byte_width),
                static_cast<unsigned>(alphabet_code),
                static_cast<unsigned>(storage_unit_byte_width));
        return results;
    }

    /*
     * Profiler dispatch fork. Picks one of the two pipelines based
     * on bioseq.aligner.enable_profiling (mirrored into
     * Minimap2Params::enable_profiling by build_params_from_gucs in
     * columnar_customscan.cppm). The two compile to genuinely
     * different code: every templated bioseq::instr::region/loop
     * call below is instantiated per Args, so the no-profiling
     * instantiation collapses to nothing.
     *
     * We use the run_pipeline lambda + a per-args constexpr-if dispatcher
     * to share the pipeline body; the templated outer scope owns the
     * ProfilerBlock + RAII installer when profiling is on.
     */
    auto run_pipeline_body =
        [batch_size, alphabet_code, storage_unit_byte_width, query_hdr,
         per_row_query_hdrs, per_row_query_mode, &target_hdrs, &query_ids,
         &base_params, &stats, scanner_pg_ctx, &results]
        <bioseq::instr::is_context_args Args>() {
        static const auto* batch_desc =
            bioseq::instr::register_region_static("BatchProcessor.process_sequences");
        auto _batch_scope = bioseq::instr::region<Args>(batch_desc);

        pgseq_t::dispatch_instance(alphabet_code, storage_unit_byte_width,
            [batch_size, alphabet_code, storage_unit_byte_width, query_hdr,
             per_row_query_hdrs, per_row_query_mode, &target_hdrs, &query_ids,
             &base_params, &stats, scanner_pg_ctx, &results]<class S>() {
            const char* _force_chunked_guc =
                GetConfigOptionByName("bioseq.test_force_chunked_scanner", NULL, true);
            const bool chunked_scanner_forced =
                _force_chunked_guc && std::strcmp(_force_chunked_guc, "on") == 0;
            // DOC0264 §9.2 / DOC0274 — test parity knob. When on, the dispatch
            // loop forces the Phase B baseline (with_lazy_in_memory). Otherwise
            // (default) and given a non-null ScannerPgContext, the loop uses
            // the Phase C `sequence_scanner_pg<S>` (PG-backed for stub targets,
            // in-memory for the query literal / inline targets).
            const char* _force_inmemory_guc =
                GetConfigOptionByName("bioseq.test_force_inmemory_scanner", NULL, true);
            const bool inmemory_scanner_forced =
                _force_inmemory_guc && std::strcmp(_force_inmemory_guc, "on") == 0;
            const bool use_scanner_pg =
                scanner_pg_ctx != nullptr
                && !inmemory_scanner_forced
                && !chunked_scanner_forced;
            JustLog("[bioseq] pg: BatchProcessor dispatching ({},{}) query_bp_size={} batch_size={} per_row_query={} use_scanner_pg={} chunked={} inmemory_forced={}",
                    static_cast<unsigned>(alphabet_code),
                    static_cast<unsigned>(storage_unit_byte_width),
                    query_hdr != nullptr ? query_hdr->bp_size : 0,
                    batch_size, per_row_query_mode,
                    use_scanner_pg, chunked_scanner_forced, inmemory_scanner_forced);

            /*
             * Query minimizers are computed once per batch for the
             * prebuilt-index path (every row reuses the same query).
             * Recompute-path rows use minimap_port(), which does its
             * own sketching; the extra allocation here is avoided
             * for those rows via the optional nested-scope.
             */
            std::optional<std::vector<MinimapSketchData>> cached_query_minimizers;
            constexpr sequence_size_t kSegmentationThresholdBp = 256'000'000;
            constexpr sequence_size_t kOpportunisticIndexMinBp = 4096;

            std::vector<std::uint8_t> parallel_done;
            std::vector<MatchResultCollection> parallel_collections;
            std::vector<Minimap2Params> parallel_params;

            auto& pool = bioseq::parallelism::GlobalPool::instance();
            const bool may_dispatch_parallel =
                !base_params.enable_profiling
                && !use_scanner_pg
                && !chunked_scanner_forced
                && bioseq::parallelism::dispatch_parallel(pool.mode())
                && pool.max_concurrency() > 1
                && batch_size >= 2;
            if (may_dispatch_parallel) {
                parallel_done.assign(batch_size, 0);
                parallel_collections.resize(batch_size);
                parallel_params.resize(batch_size);

                pool.parallel_for_with_context(0, batch_size, 1,
                    bioseq::parallelism::range_body{
                    [batch_size, alphabet_code, storage_unit_byte_width,
                     query_hdr, per_row_query_hdrs, per_row_query_mode,
                     &target_hdrs, &base_params, &parallel_done,
                     &parallel_collections, &parallel_params](
                            std::size_t lo, std::size_t hi) {
                        for (std::size_t i = lo; i < hi && i < batch_size; ++i) {
                            const pgseq_t* row_query_hdr =
                                per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
                            const pgseq_t* target_hdr = target_hdrs[i];
                            if (row_query_hdr == nullptr || target_hdr == nullptr) {
                                continue;
                            }
                            if (row_query_hdr->alphabet_code != alphabet_code
                                || row_query_hdr->storage_unit_byte_width != storage_unit_byte_width
                                || target_hdr->alphabet_code != alphabet_code
                                || target_hdr->storage_unit_byte_width != storage_unit_byte_width) {
                                continue;
                            }
                            if (target_hdr->bp_size > kSegmentationThresholdBp) {
                                continue;
                            }
                            compact_sequence_range<S> query_seq{*row_query_hdr};
                            compact_sequence_range<S> target_seq{*target_hdr};
                            const size_t min_bp = std::min<size_t>(
                                static_cast<size_t>(query_seq.bp_size()),
                                static_cast<size_t>(target_seq.bp_size()));
                            Minimap2Params params = apply_size_heuristic(base_params, min_bp);
                            auto lazy_query = bioseq::storage::make_lazy_in_memory(query_seq);
                            auto lazy_target = bioseq::storage::make_lazy_in_memory(target_seq);
                            parallel_collections[i] = minimap_port(lazy_query, lazy_target, params);
                            parallel_params[i] = params;
                            parallel_done[i] = 1;
                        }
                    }});
            }

            for (size_t i = 0; i < batch_size; ++i) {
                // DOC0276 — cancellation point at the per-row boundary. The
                // native aligner itself is NO_POSTGRES and cannot call
                // CHECK_FOR_INTERRUPTS; this lets a multi-row mmsearch honour
                // pg_cancel_backend between rows, and (with the storage-layer
                // check in fetch_payload) bounds how long a cancel waits.
                CHECK_FOR_INTERRUPTS();
                JustLogProgress("[dispatch] mmsearch row {}/{} start"
                        " | footprint={}MB avail={}MB",
                        i + 1, batch_size,
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
                if (i < parallel_done.size() && parallel_done[i] != 0) {
                    ++stats.targets_considered;
                    ++stats.targets_recomputed_minimizers;
                    stats.last_effective_params = parallel_params[i];
                    const sequence_size_t target_bp_size =
                        static_cast<sequence_size_t>(target_hdrs[i]->bp_size);
                    const std::size_t match_count = parallel_collections[i].size();
                    JustLog("[bioseq] pg: BatchProcessor row query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es) [dispatch-parallel]",
                            static_cast<int>(query_ids[i]),
                            target_bp_size,
                            stats.last_effective_params.kmer_size,
                            stats.last_effective_params.window_size,
                            stats.last_effective_params.min_chain_score,
                            stats.last_effective_params.min_seed_count,
                            stats.targets_with_prebuilt_index, match_count);
                    if (!parallel_collections[i].empty()) {
                        results[query_ids[i]] = std::move(parallel_collections[i].results);
                    }
                    continue;
                }
                const pgseq_t* row_query_hdr =
                    per_row_query_mode ? (*per_row_query_hdrs)[i] : query_hdr;
                if (row_query_hdr == nullptr) {
                    continue;
                }
                if (row_query_hdr->alphabet_code != alphabet_code
                    || row_query_hdr->storage_unit_byte_width != storage_unit_byte_width) {
                    JustLog("[bioseq] pg: query row query_id={} has typmod ({},{}) mismatching context ({},{}); treating as WHERE FALSE",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned>(row_query_hdr->alphabet_code),
                            static_cast<unsigned>(row_query_hdr->storage_unit_byte_width),
                            static_cast<unsigned>(alphabet_code),
                            static_cast<unsigned>(storage_unit_byte_width));
                    continue;
                }
                /*
                 * Build the query compact_sequence_range inside the row loop
                 * so per-row mmsearch can batch-dispatch without broadcasting
                 * one row's query to the rest of the batch.
                 */
                compact_sequence_range<S> query_seq{*row_query_hdr};
                auto lazy_query = bioseq::storage::make_lazy_in_memory(query_seq);
                const pgseq_t* target_hdr = target_hdrs[i];
                // DOC0274 — when use_scanner_pg=true AND a locator was captured
                // for this row (e.g. a stub whose materialised varlena would
                // exceed MaxAllocSize, so hydration was skipped), we still have
                // enough info to dispatch via scanner_pg's PG-backed mode. Only
                // skip when NEITHER an inline target nor a locator is available.
                const bool has_locator_for_pg =
                    use_scanner_pg
                    && scanner_pg_ctx->target_locators[i].has_value();
                if (target_hdr == nullptr && !has_locator_for_pg) {
                    continue;  // NULL seq column in this row → no match
                }
                if (target_hdr != nullptr
                    && (target_hdr->alphabet_code != alphabet_code
                        || target_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
                    /*
                     * PG's typmod coercion should have rejected the
                     * INSERT if the stored typmod didn't match the
                     * column typmod; getting here means someone
                     * wrote raw bytes via pg_catalog mischief or a
                     * future cross-typmod compare path. Skip rather
                     * than crash and let the tuple's mmsearch
                     * qualify as FALSE.
                     */
                    JustLog("[bioseq] pg: target row query_id={} has typmod ({},{}) mismatching context ({},{}); treating as WHERE FALSE",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned>(target_hdr->alphabet_code),
                            static_cast<unsigned>(target_hdr->storage_unit_byte_width),
                            static_cast<unsigned>(alphabet_code),
                            static_cast<unsigned>(storage_unit_byte_width));
                    continue;
                }
                // Construct compact_sequence_range only when target_hdr is
                // present. With a PG-backed locator we don't materialise the
                // full payload — scanner_pg streams fragment-by-fragment. Use
                // the locator's total_bp_size for the heuristic-relevant
                // min_bp comparison.
                const sequence_size_t target_bp_size = target_hdr != nullptr
                    ? static_cast<sequence_size_t>(target_hdr->bp_size)
                    : scanner_pg_ctx->target_locators[i]->total_bp_size;

                const size_t min_bp = std::min<size_t>(
                    static_cast<size_t>(query_seq.bp_size()),
                    static_cast<size_t>(target_bp_size));

                ++stats.targets_considered;

                MatchResultCollection collection;
                {
                    ++stats.targets_recomputed_minimizers;
                    Minimap2Params params = apply_size_heuristic(base_params, min_bp);
                    std::optional<std::vector<MinimapSketchData>> row_query_minimizers;
                    if (use_scanner_pg) {
                        // DOC0264 §9.2 / DOC0274 — production scanner_pg path.
                        using SP = bioseq::storage::sequence_scanner_pg<S>;
                        bioseq::storage::lazy_compact_sequence<S, SP> fq_pg{
                            bioseq::storage::make_scanner_pg_in_memory<S>(query_seq)};
                        const auto& loc_opt = scanner_pg_ctx->target_locators[i];
                        if (loc_opt.has_value()) {
                            // DOC0274 — compute last-fragment padding from
                            // total_bp_size and S's letters_per_unit. For
                            // total_bp = N, padding = ceil(N/lpu)*lpu - N.
                            constexpr std::size_t _lpu =
                                compact_sequence_range<S>::traits::letters_per_unit;
                            const std::uint64_t _total_units =
                                (loc_opt->total_bp_size + _lpu - 1) / _lpu;
                            const std::size_t _padding =
                                static_cast<std::size_t>(
                                    _total_units * _lpu - loc_opt->total_bp_size);
                            SP target_scanner(
                                scanner_pg_ctx->db_oid,
                                scanner_pg_ctx->tablespace_oid,
                                scanner_pg_ctx->main_relfilenode,
                                scanner_pg_ctx->mmsearch_seq_attno,
                                *loc_opt,
                                _padding,
                                scanner_pg_ctx->caches,
                                scanner_pg_ctx->parent_mctx);
                            bioseq::storage::lazy_compact_sequence<S, SP> ft_pg{
                                std::move(target_scanner)};
                            // DOC0279 §11.3a — ASSEMBLE from the stored
                            // per-fragment minimizer index if present
                            // (deserialise + globalise, no re-collection);
                            // else fall back to recompute (DOC0276).
                            bool assembled = false;
                            if (scanner_pg_ctx->read_index_active
                                && OidIsValid(scanner_pg_ctx->user_relid)) {
                                using IStore = bioseq::storage::indexing::IndexStorageFragmentPg;
                                const bool want_seq =
                                    scanner_pg_ctx->active_index == kMinimizersPerSequenceIndex;
                                // The stored index is ALWAYS w=10,k=15 (pinned at build); the query
                                // minimizers + the seed pipeline MUST use the same w,k (NOT the size
                                // heuristic's reduced k), exactly as the legacy prebuilt branch does.
                                Minimap2Params idx_params = base_params;
                                idx_params.kmer_size = 15; idx_params.window_size = 10;
                                if (min_bp < 50) { idx_params.min_chain_score = 10; idx_params.min_seed_count = 1; }
                                else if (min_bp < 200) { idx_params.min_chain_score = 20; idx_params.min_seed_count = 2; }
                                auto& query_minimizers =
                                    per_row_query_mode
                                        ? row_query_minimizers
                                        : cached_query_minimizers;
                                auto ensure_q =
                                    [&query_minimizers, &query_seq, &idx_params] {
                                    if (!query_minimizers.has_value()) {
                                        sketch_collector qc;
                                        collect_minimizers<minimap_int_hasher, MinimapSketchData>(
                                            qc, query_seq, query_seq.begin(), query_seq.end(),
                                            static_cast<size_t>(idx_params.window_size),
                                            static_cast<size_t>(idx_params.kmer_size));
                                        query_minimizers.emplace(std::move(qc).extract_sketches_data());
                                    }
                                };
                                auto make_cache_key =
                                    [scanner_pg_ctx, loc_opt, &query_minimizers, &idx_params, i](
                                            std::string_view index_name,
                                            sequence_size_t clip_lo_bp,
                                            sequence_size_t clip_hi_bp) {
                                    const auto query_span = std::span<const MinimapSketchData>(
                                        query_minimizers->data(), query_minimizers->size());
                                    return bioseq::storage::indexing::AssembledIndexKey{
                                        .db_oid = scanner_pg_ctx->db_oid,
                                        .user_relid = scanner_pg_ctx->user_relid,
                                        .tablespace_oid = scanner_pg_ctx->tablespace_oid,
                                        .main_relfilenode = scanner_pg_ctx->main_relfilenode,
                                        .column_attno = scanner_pg_ctx->mmsearch_seq_attno,
                                        .index_name_hash = bioseq::storage::indexing::index_name_hash(
                                            scanner_pg_ctx->db_oid,
                                            scanner_pg_ctx->tablespace_oid,
                                            scanner_pg_ctx->main_relfilenode,
                                            scanner_pg_ctx->mmsearch_seq_attno,
                                            index_name),
                                        .query_key = static_cast<std::uint64_t>(
                                            scanner_pg_ctx->target_rownos[i]),
                                        .query_signature =
                                            bioseq::storage::indexing::query_minimizer_signature(
                                                query_span,
                                                static_cast<std::uint32_t>(idx_params.kmer_size),
                                                static_cast<std::uint32_t>(idx_params.window_size),
                                                static_cast<std::uint32_t>(idx_params.max_occ)),
                                        .clip_lo_bp = clip_lo_bp,
                                        .clip_hi_bp = clip_hi_bp,
                                    };
                                };
                                // DOC0279 §11.3 — route by the table's ACTIVE read index
                                // (set_active_read_index / resolve_active_read_index, default
                                // minimizers_per_fragment). Per-row presence still gates: an
                                // active index not materialised for THIS row recomputes.
                                if (want_seq) {
                                // §11.3b — per-sequence merged index: query-relevant buckets
                                // only + exact global max_occ.
                                auto seq_store = bioseq::storage::indexing::open_fragment_storage(
                                    scanner_pg_ctx->user_relid, scanner_pg_ctx->db_oid,
                                    scanner_pg_ctx->tablespace_oid, scanner_pg_ctx->main_relfilenode,
                                    scanner_pg_ctx->mmsearch_seq_attno, kMinimizersPerSequenceIndex,
                                    *loc_opt, (uint64_t)scanner_pg_ctx->target_rownos[i],
                                    scanner_pg_ctx->caches, scanner_pg_ctx->parent_mctx);
                                if (seq_store.number_of_chunks(bioseq::indexing::DIRECTORY_SUB_KEY) >= 1) {
                                    ensure_q();
                                    const auto cache_key = make_cache_key(
                                        kMinimizersPerSequenceIndex, 0, loc_opt->total_bp_size);
                                    if (auto hit = bioseq::storage::indexing::lookup_assembled_index(cache_key)) {
                                        collection = minimap2_port_with_index(
                                            fq_pg, ft_pg, *query_minimizers, *hit->index, idx_params);
                                        JustLog("[bioseq] index: assembled cache hit index=minimizers_per_sequence row={} target_bp={}",
                                                static_cast<unsigned long long>(scanner_pg_ctx->target_rownos[i]),
                                                static_cast<unsigned long long>(loc_opt->total_bp_size));
                                    } else {
                                        bioseq::indexing::MinimizerAssemblyScratch sq_scratch;
                                        auto idx = bioseq::indexing::MinimizerSequenceMergedIndex<IStore>::assemble_for_query(
                                            seq_store,
                                            std::span<const MinimapSketchData>(query_minimizers->data(),
                                                                               query_minimizers->size()),
                                            loc_opt->total_bp_size, idx_params.max_occ, sq_scratch);
                                        collection = minimap2_port_with_index(
                                            fq_pg, ft_pg, *query_minimizers, idx, idx_params);
                                        bioseq::storage::indexing::store_assembled_index(
                                            cache_key, std::move(idx));
                                    }
                                    assembled = true;
                                }
                                } else {
                                auto frag_store = bioseq::storage::indexing::open_fragment_storage(
                                    scanner_pg_ctx->user_relid, scanner_pg_ctx->db_oid,
                                    scanner_pg_ctx->tablespace_oid, scanner_pg_ctx->main_relfilenode,
                                    scanner_pg_ctx->mmsearch_seq_attno, kMinimizersPerFragmentIndex,
                                    *loc_opt, (uint64_t)scanner_pg_ctx->target_rownos[i],
                                    scanner_pg_ctx->caches, scanner_pg_ctx->parent_mctx);
                                if (frag_store.number_of_chunks(bioseq::indexing::DIRECTORY_SUB_KEY) >= 1) {
                                    ensure_q();
                                    const sequence_size_t total_bp = loc_opt->total_bp_size;
                                    const std::size_t frag_units = SP::fragment_storage_units;
                                    const std::size_t a_lpu = compact_sequence_range<S>::traits::letters_per_unit;
                                    const sequence_size_t frag_bp = (sequence_size_t)frag_units * a_lpu;
                                    const std::size_t size_u = (std::size_t)((total_bp + a_lpu - 1) / a_lpu);
                                    bioseq::indexing::MinimizerAssemblyScratch scratch;
                                    auto assemble_one =
                                        [frag_units, a_lpu, total_bp, frag_bp,
                                         &frag_store, &scratch](std::size_t cs_u, std::size_t ce_u) {
                                        const std::size_t frag_lo = cs_u / frag_units;
                                        const std::size_t frag_hi = (ce_u + frag_units - 1) / frag_units;
                                        const sequence_size_t cs_bp = (sequence_size_t)cs_u * a_lpu;
                                        const sequence_size_t ce_bp = std::min((sequence_size_t)ce_u * a_lpu, total_bp);
                                        return bioseq::indexing::MinimizersPerFragmentIndex<IStore>::assemble_segment(
                                            frag_store, frag_lo, frag_hi, frag_bp, total_bp, cs_bp, ce_bp, scratch);
                                    };
                                    auto run_with_segment_index =
                                        [&make_cache_key, &assemble_one, &fq_pg, &ft_pg,
                                         &query_minimizers, &idx_params](
                                                sequence_size_t clip_lo_bp,
                                                sequence_size_t clip_hi_bp,
                                                std::size_t cs_u,
                                                std::size_t ce_u) {
                                        const auto cache_key = make_cache_key(
                                            kMinimizersPerFragmentIndex, clip_lo_bp, clip_hi_bp);
                                        if (auto hit = bioseq::storage::indexing::lookup_assembled_index(cache_key)) {
                                            return minimap2_port_with_index(
                                                fq_pg, ft_pg, *query_minimizers, *hit->index, idx_params);
                                        }
                                        auto idx = assemble_one(cs_u, ce_u);
                                        MatchResultCollection out = minimap2_port_with_index(
                                            fq_pg, ft_pg, *query_minimizers, idx, idx_params);
                                        bioseq::storage::indexing::store_assembled_index(
                                            cache_key, std::move(idx));
                                        return out;
                                    };
                                    if (total_bp <= kSegmentationThresholdBp) {
                                        collection = run_with_segment_index(
                                            0, total_bp, 0, size_u);
                                    } else {
                                        std::vector<MatchResult> merged;
                                        const sequence_size_t query_len = query_seq.bp_size();
                                        for (auto sr : pipe::segment_unit_ranges(total_bp, query_len, a_lpu)) {
                                            MatchResultCollection seg_res =
                                                run_with_segment_index(
                                                    sr.core_lo_bp, sr.core_hi_bp, sr.cs_u, sr.ce_u);
                                            for (auto& r : seg_res.results)
                                                if (r.target_start >= sr.core_lo_bp && r.target_start < sr.core_hi_bp)
                                                    merged.push_back(std::move(r));
                                        }
                                        collection = MatchResultCollection{std::move(merged)};
                                    }
                                    assembled = true;
                                }
                                }  // close the per-fragment else branch
                                if (!assembled
                                    && !want_seq
                                    && loc_opt->total_bp_size >= kOpportunisticIndexMinBp) {
                                    const std::uint64_t peak_bytes =
                                        bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
                                            static_cast<std::uint64_t>(loc_opt->total_bp_size));
                                    auto build_permit =
                                        bioseq::parallelism::MemoryAdmissionController::instance()
                                            .try_reserve(peak_bytes);
                                    if (build_permit) {
                                        ensure_q();
                                        OpportunisticFragmentIndexStorage ram_store;
                                        bioseq::indexing::MinimizersPerFragmentIndex<
                                            OpportunisticFragmentIndexStorage>::build(
                                                ram_store, ft_pg);

                                        const bool staged =
                                            stage_opportunistic_fragment_store_for_async_persist(
                                                *scanner_pg_ctx,
                                                *loc_opt,
                                                ram_store,
                                                static_cast<std::uint64_t>(
                                                    scanner_pg_ctx->target_rownos[i]));
                                        JustLog("[bioseq] index: opportunistic RAM-first index row={} target_bp={} chunks={} peak_bytes={} async_staged={}",
                                                static_cast<unsigned long long>(
                                                    scanner_pg_ctx->target_rownos[i]),
                                                static_cast<unsigned long long>(
                                                    loc_opt->total_bp_size),
                                                ram_store.chunks.size(),
                                                static_cast<unsigned long long>(peak_bytes),
                                                staged);

                                        const sequence_size_t total_bp = loc_opt->total_bp_size;
                                        const std::size_t frag_units = SP::fragment_storage_units;
                                        const std::size_t a_lpu =
                                            compact_sequence_range<S>::traits::letters_per_unit;
                                        const sequence_size_t frag_bp =
                                            static_cast<sequence_size_t>(frag_units) * a_lpu;
                                        const std::size_t size_u =
                                            static_cast<std::size_t>((total_bp + a_lpu - 1) / a_lpu);
                                        bioseq::indexing::MinimizerAssemblyScratch scratch;
                                        auto assemble_ram_one =
                                            [frag_units, a_lpu, total_bp, frag_bp,
                                             &ram_store, &scratch](
                                                    std::size_t cs_u,
                                                    std::size_t ce_u) {
                                            const std::size_t frag_lo = cs_u / frag_units;
                                            const std::size_t frag_hi =
                                                (ce_u + frag_units - 1) / frag_units;
                                            const sequence_size_t cs_bp =
                                                static_cast<sequence_size_t>(cs_u) * a_lpu;
                                            const sequence_size_t ce_bp =
                                                std::min(
                                                    static_cast<sequence_size_t>(ce_u) * a_lpu,
                                                    total_bp);
                                            return bioseq::indexing::MinimizersPerFragmentIndex<
                                                OpportunisticFragmentIndexStorage>::assemble_segment(
                                                    ram_store,
                                                    frag_lo,
                                                    frag_hi,
                                                    frag_bp,
                                                    total_bp,
                                                    cs_bp,
                                                    ce_bp,
                                                    scratch);
                                        };
                                        auto run_with_ram_index =
                                            [&make_cache_key, &assemble_ram_one, &fq_pg, &ft_pg,
                                             &query_minimizers, &idx_params](
                                                    sequence_size_t clip_lo_bp,
                                                    sequence_size_t clip_hi_bp,
                                                    std::size_t cs_u,
                                                    std::size_t ce_u) {
                                            const auto cache_key = make_cache_key(
                                                kMinimizersPerFragmentIndex,
                                                clip_lo_bp,
                                                clip_hi_bp);
                                            if (auto hit =
                                                    bioseq::storage::indexing::lookup_assembled_index(
                                                        cache_key)) {
                                                return minimap2_port_with_index(
                                                    fq_pg,
                                                    ft_pg,
                                                    *query_minimizers,
                                                    *hit->index,
                                                    idx_params);
                                            }
                                            auto idx = assemble_ram_one(cs_u, ce_u);
                                            MatchResultCollection out =
                                                minimap2_port_with_index(
                                                    fq_pg,
                                                    ft_pg,
                                                    *query_minimizers,
                                                    idx,
                                                    idx_params);
                                            bioseq::storage::indexing::store_assembled_index(
                                                cache_key, std::move(idx));
                                            return out;
                                        };
                                        if (total_bp <= kSegmentationThresholdBp) {
                                            collection = run_with_ram_index(
                                                0, total_bp, 0, size_u);
                                        } else {
                                            std::vector<MatchResult> merged;
                                            const sequence_size_t query_len = query_seq.bp_size();
                                            for (auto sr : pipe::segment_unit_ranges(
                                                     total_bp, query_len, a_lpu)) {
                                                MatchResultCollection seg_res =
                                                    run_with_ram_index(
                                                        sr.core_lo_bp,
                                                        sr.core_hi_bp,
                                                        sr.cs_u,
                                                        sr.ce_u);
                                                for (auto& r : seg_res.results) {
                                                    if (r.target_start >= sr.core_lo_bp
                                                        && r.target_start < sr.core_hi_bp) {
                                                        merged.push_back(std::move(r));
                                                    }
                                                }
                                            }
                                            collection = MatchResultCollection{std::move(merged)};
                                        }
                                        assembled = true;
                                    } else {
                                        JustLog("[bioseq] index: opportunistic RAM-first index skipped row={} target_bp={} peak_bytes={} (not admitted)",
                                                static_cast<unsigned long long>(
                                                    scanner_pg_ctx->target_rownos[i]),
                                                static_cast<unsigned long long>(
                                                    loc_opt->total_bp_size),
                                                static_cast<unsigned long long>(peak_bytes));
                                    }
                                }
                            }
                            if (!assembled) {
                                const bool try_resident_segment_workers =
                                    target_bp_size > kSegmentationThresholdBp
                                    && !base_params.enable_profiling
                                    && bioseq::parallelism::task_parallel(pool.mode())
                                    && pool.max_concurrency() > 1;
                                if (try_resident_segment_workers) {
                                    auto resident =
                                        try_materialize_target_for_segment_workers(
                                            ft_pg,
                                            static_cast<std::uint64_t>(
                                                scanner_pg_ctx->target_rownos[i]));
                                    if (resident.has_value()) {
                                        auto resident_target =
                                            bioseq::storage::make_lazy_in_memory(
                                                resident->range);
                                        const bool can_defer_recompute_persist =
                                            bioseq_recompute_async_persist
                                            && bioseq_index_async_persist
                                            && OidIsValid(scanner_pg_ctx->user_relid)
                                            && params.kmer_size == 15
                                            && params.window_size == 10;
                                        const auto deferred_persist_choice =
                                            can_defer_recompute_persist
                                                ? choose_deferred_minimizer_persist_index(
                                                    *scanner_pg_ctx)
                                                : std::optional<DeferredMinimizerPersistChoice>{};
                                        if (deferred_persist_choice.has_value()) {
                                            const auto rowno =
                                                static_cast<std::uint64_t>(
                                                    scanner_pg_ctx->target_rownos[i]);
                                            collection =
                                                run_recompute_with_deferred_minimizer_persist(
                                                    lazy_query,
                                                    resident_target,
                                                    params,
                                                    *scanner_pg_ctx,
                                                    *loc_opt,
                                                    rowno,
                                                    *deferred_persist_choice);
                                        } else {
                                            collection = minimap_port_segmented(
                                                lazy_query, resident_target, params);
                                            if (bioseq_recompute_async_persist
                                                && (params.kmer_size != 15
                                                    || params.window_size != 10)) {
                                                JustLog("[bioseq] index: recompute async-persist skipped row={} target_bp={} because active params are k={} w={} but persisted minimizer indexes are pinned to k=15 w=10",
                                                        static_cast<unsigned long long>(
                                                            scanner_pg_ctx->target_rownos[i]),
                                                        static_cast<unsigned long long>(
                                                            loc_opt->total_bp_size),
                                                        params.kmer_size,
                                                        params.window_size);
                                            } else if (can_defer_recompute_persist) {
                                                JustLog("[bioseq] index: recompute async-persist skipped row={} target_bp={} because no minimizer index is enabled for UPDATE_ON_INSERT",
                                                        static_cast<unsigned long long>(
                                                            scanner_pg_ctx->target_rownos[i]),
                                                        static_cast<unsigned long long>(
                                                            loc_opt->total_bp_size));
                                            }
                                        }
                                    } else {
                                        collection = minimap_port_segmented(
                                            fq_pg, ft_pg, params);
                                    }
                                } else {
                                    collection =
                                        (target_bp_size > kSegmentationThresholdBp)
                                            ? minimap_port_segmented(fq_pg, ft_pg, params)
                                            : minimap_port(fq_pg, ft_pg, params);
                                }
                            }
                        } else {
                            compact_sequence_range<S> target_seq{*target_hdr};
                            bioseq::storage::lazy_compact_sequence<S, SP> ft_pg{
                                bioseq::storage::make_scanner_pg_in_memory<S>(target_seq)};
                            collection =
                                (target_bp_size > kSegmentationThresholdBp)
                                    ? minimap_port_segmented(fq_pg, ft_pg, params)
                                    : minimap_port(fq_pg, ft_pg, params);
                        }
                    } else if (target_hdr != nullptr) {
                        compact_sequence_range<S> target_seq{*target_hdr};
                        auto lazy_target = bioseq::storage::make_lazy_in_memory(target_seq);
                        if (chunked_scanner_forced) {
                            using chunked = bioseq::storage::sequence_scanner_test_chunked<
                                typename compact_sequence_range<S>::traits::storage_unit,
                                bioseq::storage::SCANNER_SEQUENCE_FRAGMENT_SIZE_ALIGNMENT>;
                            bioseq::storage::lazy_compact_sequence<S, chunked> fq{chunked::from_range(query_seq)};
                            bioseq::storage::lazy_compact_sequence<S, chunked> ft{chunked::from_range(target_seq)};
                            collection =
                                (target_bp_size > kSegmentationThresholdBp)
                                    ? minimap_port_segmented(fq, ft, params)
                                    : minimap_port(fq, ft, params);
                        } else {
                            collection =
                                (target_bp_size > kSegmentationThresholdBp)
                                    ? minimap_port_segmented(lazy_query, lazy_target, params)
                                    : minimap_port(lazy_query, lazy_target, params);
                        }
                    } else if (scanner_pg_ctx != nullptr
                               && scanner_pg_ctx->target_locators[i].has_value()) {
                        // DOC0274 (round-2 revert): inmemory_forced=ON cannot satisfy its
                        // force-baseline semantic on > MaxAllocSize stubs; raise.
                        pg_log(ERROR,
                            "bioseq.test_force_inmemory_scanner=on: row {} target stub "
                            "({} bp) exceeds MaxAllocSize and cannot be materialised "
                            "for the Phase B baseline. Either set the GUC to off (use "
                            "the Phase C scanner_pg path which streams fragments) or "
                            "wait for Phase D D-VARLENA. See DOC0267.",
                            static_cast<int>(query_ids[i]),
                            static_cast<unsigned long long>(target_bp_size));
                    } else {
                        JustLog("[bioseq] pg: row {} has no inline target and no scanner_pg locator; treating as WHERE FALSE",
                                static_cast<int>(query_ids[i]));
                        continue;
                    }
                    stats.last_effective_params = params;
                }

                JustLog("[bioseq] pg: BatchProcessor row query_id={} target_bp_size={} k={} w={} min_chain_score={} min_seed_count={} prebuilt_index={} produced {} match(es)",
                        static_cast<int>(query_ids[i]),
                        target_bp_size,
                        stats.last_effective_params.kmer_size,
                        stats.last_effective_params.window_size,
                        stats.last_effective_params.min_chain_score,
                        stats.last_effective_params.min_seed_count,
                        stats.targets_with_prebuilt_index, collection.size());
                if (!collection.empty()) {
                    results[query_ids[i]] = std::move(collection.results);
                }
            }
        });
    };  // run_pipeline_body

    if (base_params.enable_profiling) {
        /*
         * Profiling pipeline. The ProfilerBlock is the persistent
         * process-wide singleton (see global_state.cppm); aggregates
         * accumulate across every batch in the current scan. The
         * one-shot summary fires at scan end inside
         * ColumnarScan_EndCustomScan (Part 48.5/48.8 of the
         * design doc) — emitting it here would produce one summary
         * per batch and accumulate duplicates on multi-batch scans.
         */
        // Install both the full (with PMU counters) and timing-only
        // profiler blocks. The non-templated `region(d)` / `loop(d)`
        // call sites in chaining / kmers / minimap_port are pinned to
        // context_args_profiling_timing_only so they short-circuit
        // unless we also install that block here. Postgres production
        // monitoring continues to use the full block (counters on)
        // for any explicitly-templated `region<full_args>(d)` calls.
        using Args = bioseq::instr::context_args_profiling_full;
        using TimingArgs = bioseq::instr::context_args_profiling_timing_only;
        auto& profiler_block =
            bioseq::instr::persistent_profiler_block<Args>();
        auto& timing_profiler_block =
            bioseq::instr::persistent_profiler_block<TimingArgs>();
        bioseq::instr::installed_profiler_scope<Args> guard(profiler_block);
        bioseq::instr::installed_profiler_scope<TimingArgs> timing_guard(timing_profiler_block);
        run_pipeline_body.template operator()<Args>();
    } else {
        using Args = bioseq::instr::context_args_no_profiling;
        run_pipeline_body.template operator()<Args>();
    }

    return results;
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
