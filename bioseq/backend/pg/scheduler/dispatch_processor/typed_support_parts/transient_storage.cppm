/**
 * @file typed_support_parts/transient_storage.cppm
 * @brief Defines transient fragment and minimizer-sequence storage owners.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}
#include <libassert/assert.hpp>
#include <unistd.h>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.transient_storage;

import std;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.support;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.pipeline.pipe;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.scheduler.persist_target;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** In-memory byte payload for one opportunistic fragment-index chunk. */
struct opportunistic_index_chunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::vector<char> bytes{};
};

/** Transient in-memory chunked storage for opportunistic minimizer fragment indexes. */
struct opportunistic_fragment_index_storage {
    std::map<bioseq::indexing::index_sub_key_t, std::vector<std::size_t>> plans{};
    std::vector<opportunistic_index_chunk> chunks{};

    /** Raises a PostgreSQL data-corruption error and does not return. */
    [[noreturn]] void index_error(std::string_view msg) const {
        bioseq::postgres::support::invoke_pg_as_cpp([msg] {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq opportunistic index: %.*s",
                            static_cast<int>(msg.size()),
                            msg.data())));
        });
        pg_unreachable();
    }

    /** Validates encoded minimizer blob header, layout revision, and total byte size. */
    [[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header
    validate_minimizer_blob_or_error(
        const char* blob_data,
        std::size_t bytes,
        std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using header_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header;
        using bucket_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_bucket;
        if (bytes < sizeof(header_type)) {
            index_error("minimizer blob truncated header");
        }
        header_type header{};
        std::memcpy(&header, blob_data, sizeof(header_type));
        if (header.magic != bioseq::indexing::minimizer_hashmap_fragment_index::k_encoded_magic ||
            header.layout_revision !=
                bioseq::indexing::minimizer_hashmap_fragment_index::k_encoded_layout_revision) {
            index_error("minimizer blob magic/layout");
        }
        const std::size_t expected_bytes = sizeof(header_type) +
                                           sizeof(bucket_type) * header.bucket_count +
                                           sizeof(std::uint64_t) * header.total_positions;
        if (bytes != expected_bytes) {
            index_error("minimizer blob size mismatch");
        }
        if (expected_bucket_count && header.bucket_count != *expected_bucket_count) {
            index_error("minimizer blob bucket_count mismatch");
        }
        return header;
    }

    /** Rejects directory blobs shorter than one int64. */
    void validate_minimizer_directory_or_error(const char*, std::size_t bytes) const {
        if (bytes < sizeof(std::int64_t)) {
            index_error("directory truncated");
        }
    }

    /** Rejects chunk metadata when stored byte size is below @p min_bytes. */
    void validate_minimizer_chunk_metadata_or_error(bioseq::indexing::index_sub_key_t sub_key,
                                                    std::size_t chunk_no,
                                                    std::size_t min_bytes,
                                                    std::size_t) const {
        if (chunk_byte_size(sub_key, chunk_no) < min_bytes) {
            index_error("chunk metadata below minimum size");
        }
    }

    /** Returns how many chunks belong to @p sub_key. */
    [[nodiscard]] std::size_t number_of_chunks(bioseq::indexing::index_sub_key_t sub_key) const {
        std::size_t chunk_count = 0;
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++chunk_count;
            }
        }
        return chunk_count;
    }

    /** Counts distinct sub-keys in chunk append order. */
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t sub_key_count = 0;
        bioseq::indexing::index_sub_key_t last_sub_key =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have_sub_key = false;
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (!have_sub_key || chunk.sub_key != last_sub_key) {
                ++sub_key_count;
                last_sub_key = chunk.sub_key;
                have_sub_key = true;
            }
        }
        return sub_key_count;
    }

    /** Returns stored byte length for a chunk, or zero when missing. */
    [[nodiscard]] std::size_t chunk_byte_size(bioseq::indexing::index_sub_key_t sub_key,
                                              std::size_t chunk_no) const {
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk.bytes.size();
            }
        }
        return 0;
    }

    /** Returns a typed view into chunk bytes or reports a missing chunk. */
    template <class p_value>
    [[nodiscard]] const p_value* read_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                            std::size_t chunk_no) {
        for (opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return reinterpret_cast<const p_value*>(chunk.bytes.data());
            }
        }
        index_error("chunk missing");
    }

    /** Plans fixed-size chunks that cover @p bytes_count for @p sub_key. */
    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t chunk_count =
            bytes_count == 0 ? 1
                             : (bytes_count + bioseq::indexing::k_max_index_chunk_size_bytes - 1) /
                                   bioseq::indexing::k_max_index_chunk_size_bytes;
        std::vector<std::size_t> chunk_plan(chunk_count);
        std::size_t remaining_bytes = bytes_count;
        for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            const std::size_t chunk_bytes =
                std::min(remaining_bytes, bioseq::indexing::k_max_index_chunk_size_bytes);
            chunk_plan[chunk_index] = chunk_bytes;
            remaining_bytes -= chunk_bytes;
        }
        plans[sub_key] = std::move(chunk_plan);
    }

    /** Replaces the per-chunk byte plan for @p sub_key. */
    void resize_chunks(bioseq::indexing::index_sub_key_t sub_key,
                       std::span<const std::size_t> per_chunk_bytes) {
        plans[sub_key] = std::vector<std::size_t>(per_chunk_bytes.begin(), per_chunk_bytes.end());
    }

    /** No-op prefetch hook required by the storage concept. */
    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    /** Materializes planned chunks and fills each via @p writer. */
    template <class p_value, class p_function>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        auto plan_iterator = plans.find(sub_key);
        if (plan_iterator == plans.end()) {
            index_error("write_to_each without resize");
        }
        const std::vector<std::size_t>& chunk_plan = plan_iterator->second;
        for (std::size_t chunk_no = 0; chunk_no < chunk_plan.size();) {
            const std::size_t payload_bytes = chunk_plan[chunk_no];
            if (payload_bytes % sizeof(p_value) != 0) {
                index_error("chunk payload is not divisible by the value type size");
            }
            opportunistic_index_chunk chunk{};
            chunk.sub_key = sub_key;
            chunk.chunk_no = static_cast<std::uint32_t>(chunk_no);
            chunk.bytes.assign(payload_bytes, char{0});
            const std::size_t next = writer(chunk_no,
                                            reinterpret_cast<p_value*>(chunk.bytes.data()),
                                            payload_bytes / sizeof(p_value));
            if (next < chunk_no) {
                index_error("writer moved backwards");
            }
            chunks.push_back(std::move(chunk));
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
        plans.erase(plan_iterator);
    }

    /** Invokes @p callback for chunks whose sub-key lies in [@p lower_bound, @p upper_bound). */
    template <class p_function>
    void read_chunks_in_range(bioseq::indexing::index_sub_key_t lower_bound,
                              bioseq::indexing::index_sub_key_t upper_bound,
                              p_function&& callback) {
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key >= lower_bound && chunk.sub_key < upper_bound) {
                callback(chunk.sub_key, chunk.bytes.data(), chunk.bytes.size());
            }
        }
    }

    /** Calls @p callback once per distinct sub-key in chunk order. */
    void for_each_sub_key(auto&& callback) const {
        bioseq::indexing::index_sub_key_t last_sub_key =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have_sub_key = false;
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (!have_sub_key || chunk.sub_key != last_sub_key) {
                callback(chunk.sub_key);
                last_sub_key = chunk.sub_key;
                have_sub_key = true;
            }
        }
    }

    /** Drops all chunk payloads and pending resize plans. */
    void clear_entity() {
        plans.clear();
        chunks.clear();
    }

    /** Clears transient storage and ignores the name argument. */
    void clear(std::string_view) {
        clear_entity();
    }

    /** Removes chunks for sub-keys in the inclusive [@p first_sub_key, @p last_sub_key] range. */
    void discard_sub_keys(bioseq::indexing::index_sub_key_t first_sub_key,
                          bioseq::indexing::index_sub_key_t last_sub_key) {
        chunks.erase(std::remove_if(
                         chunks.begin(),
                         chunks.end(),
                         [first_sub_key, last_sub_key](const opportunistic_index_chunk& chunk) {
                             return chunk.sub_key >= first_sub_key && chunk.sub_key <= last_sub_key;
                         }),
                     chunks.end());
    }

    /** Reports an index error because this operation is unsupported. */
    [[nodiscard]] std::size_t
    spill_sorted_run(std::span<const std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    /** Reports an index error because this operation is unsupported. */
    [[nodiscard]] std::size_t
    spill_sorted_run_owned(std::vector<std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    /** Releases @p permit and reports an index error because this operation is unsupported. */
    [[nodiscard]] std::size_t
    spill_sorted_run_owned_admitted(std::vector<std::pair<std::uint64_t, std::uint64_t>>,
                                    std::uint64_t,
                                    bioseq::parallelism::memory_permit permit) {
        permit.release();
        index_error("external sort spill is not supported by opportunistic storage");
    }

    /** Reports an index error because this operation is unsupported. */
    void merge_runs(auto&&) {
        index_error("external sort merge is not supported by opportunistic storage");
    }

    /** Does nothing because opportunistic fragment storage holds no sorted runs. */
    void release_resident_runs_noexcept() noexcept {}

    /** Does nothing because opportunistic fragment storage holds no sorted runs. */
    void release_runs_noexcept() noexcept {}

    /** Does nothing because opportunistic fragment storage holds no sorted runs. */
    void release_runs() {}

    /** Always returns zero because external sort is unsupported. */
    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return 0;
    }
};

// Transient minimizer-sequence storage backing a real no-index per-sequence
// build. Unlike opportunistic_fragment_index_storage (which refuses external-sort
// spill), this delegates spill/merge to a real sorted_pair_run_store so it satisfies
// is_external_sort_spill_storage + is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
// that direct_minimizer_bucket_index<p_sequence> requires. No persisted sidecar rows.
/** In-memory byte payload for one opportunistic per-sequence index chunk. */
struct opportunistic_sequence_chunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::size_t chunk_no = 0;
    std::vector<char> bytes;
};

struct opportunistic_sequence_index_storage {
    /** Key/value pair spilled during external sort. */
    using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;

    std::vector<opportunistic_sequence_chunk> chunks;
    bioseq::storage::indexing::sorted_pair_run_store sorted_runs;
    std::size_t run_buffer_bytes_value = std::size_t{256} << 20;

    /** Returns the configured in-memory buffer budget for sorted runs. */
    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return run_buffer_bytes_value;
    }

    /** Clears chunk payloads and releases spilled sorted runs. */
    void clear_entity() {
        chunks.clear();
        sorted_runs.release_runs();
    }

    // Required by is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
    // but unused on the transient path.
    void clear(std::string_view) {
        clear_entity();
    }

    /** Resizes or creates chunks for @p sub_key from @p chunk_plan. */
    void resize_chunks(bioseq::indexing::index_sub_key_t sub_key,
                       std::span<const std::size_t> chunk_plan) {
        for (std::size_t chunk_no = 0; chunk_no < chunk_plan.size(); ++chunk_no) {
            auto& chunk = ensure_chunk(sub_key, chunk_no);
            chunk.bytes.resize(chunk_plan[chunk_no]);
        }
    }

    /** Stores all @p bytes in a single chunk for @p sub_key. */
    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes) {
        std::array<std::size_t, 1> chunk_plan{bytes};
        resize_chunks(sub_key, std::span<const std::size_t>{chunk_plan.data(), chunk_plan.size()});
    }

    /** No-op prefetch hook required by the storage concept. */
    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    /** Invokes @p writer for each existing chunk of @p sub_key. */
    template <class p_value, class p_function>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        for (opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key != sub_key) {
                continue;
            }
            const std::size_t cap = chunk.bytes.size() / sizeof(p_value);
            writer(chunk.chunk_no, reinterpret_cast<p_value*>(chunk.bytes.data()), cap);
        }
    }

    /** Alias for @c write_to_each on the transient per-sequence path. */
    template <class p_value, class p_function>
    void write_to(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        write_to_each<p_value>(sub_key, std::forward<p_function>(writer));
    }

    /** Returns raw bytes for a chunk. */
    [[nodiscard]] const char* read_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                         std::size_t chunk_no) const {
        return chunk_ref(sub_key, chunk_no).bytes.data();
    }

    /** Returns a typed view into chunk bytes. */
    template <class p_value>
    [[nodiscard]] const p_value* read_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                            std::size_t chunk_no) const {
        return reinterpret_cast<const p_value*>(chunk_ref(sub_key, chunk_no).bytes.data());
    }

    /** Returns stored byte length for a chunk. */
    [[nodiscard]] std::size_t chunk_byte_size(bioseq::indexing::index_sub_key_t sub_key,
                                              std::size_t chunk_no) const {
        return chunk_ref(sub_key, chunk_no).bytes.size();
    }

    /** Returns how many chunks belong to @p sub_key. */
    [[nodiscard]] std::size_t number_of_chunks(bioseq::indexing::index_sub_key_t sub_key) const {
        std::size_t chunk_count = 0;
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++chunk_count;
            }
        }
        return chunk_count;
    }

    // Reports the distinct sub-keys required by the storage concept.
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t sub_key_count = 0;
        for_each_sub_key([&sub_key_count](bioseq::indexing::index_sub_key_t) { ++sub_key_count; });
        return sub_key_count;
    }

    /** Invokes @p callback for each distinct sub-key. */
    template <class p_function> void for_each_sub_key(p_function&& callback) const {
        bioseq::indexing::index_sub_key_t last_sub_key = 0;
        bool have_last_sub_key = false;
        // chunks are appended in build order; emit each distinct sub_key once.
        std::vector<bioseq::indexing::index_sub_key_t> seen_sub_keys;
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (have_last_sub_key && chunk.sub_key == last_sub_key) {
                continue;
            }
            bool already_seen = false;
            for (const auto key : seen_sub_keys) {
                if (key == chunk.sub_key) {
                    already_seen = true;
                    break;
                }
            }
            if (!already_seen) {
                seen_sub_keys.push_back(chunk.sub_key);
                callback(chunk.sub_key);
            }
            last_sub_key = chunk.sub_key;
            have_last_sub_key = true;
        }
    }

    /** Spills a sorted key/value span and returns the run index. */
    [[nodiscard]] std::size_t spill_sorted_run(std::span<const sorted_pair_t> sorted) {
        return sorted_runs.spill_sorted_run(sorted);
    }

    [[nodiscard]] std::size_t spill_sorted_run_owned(std::vector<sorted_pair_t> sorted) {
        // The transient no-index minimizer-sequence path immediately enters the next target
        // next target range. Keep these runs file-backed so resident pair
        // vectors do not consume the admission budget needed by the nested GPU
        // sketch dispatch.
        return sorted_runs.spill_sorted_run(
            std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
    }

    /** Spills an owned sorted run after admission and releases @p permit. */
    [[nodiscard]] std::size_t
    spill_sorted_run_owned_admitted(std::vector<sorted_pair_t> sorted,
                                    std::uint64_t admitted_bytes,
                                    bioseq::parallelism::memory_permit permit) {
        (void)admitted_bytes;
        const std::size_t run_index = sorted_runs.spill_sorted_run(
            std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
        permit.release();
        return run_index;
    }

    /** Merges spilled runs and forwards each k-mer to @p on_kmer. */
    template <class p_function> void merge_runs(p_function&& on_kmer) {
        sorted_runs.merge_runs(std::forward<p_function>(on_kmer));
    }

    /** Releases resident sorted-run buffers without throwing. */
    void release_resident_runs_noexcept() noexcept {
        sorted_runs.release_resident_runs_noexcept();
    }

    /** Releases all sorted runs and their backing storage. */
    void release_runs() {
        sorted_runs.release_runs();
    }

    /** Releases all sorted runs without throwing. */
    void release_runs_noexcept() noexcept {
        sorted_runs.release_runs_noexcept();
    }

    /** Invokes @p callback for every chunk in [@p range_begin, @p range_end). */
    template <class p_function>
    void read_chunks_in_range(bioseq::indexing::index_sub_key_t range_begin,
                              bioseq::indexing::index_sub_key_t range_end,
                              p_function&& callback) const {
        for (bioseq::indexing::index_sub_key_t sub_key = range_begin; sub_key < range_end;
             ++sub_key) {
            for (const opportunistic_sequence_chunk& chunk : chunks) {
                if (chunk.sub_key == sub_key) {
                    callback(sub_key, chunk.bytes.data(), chunk.bytes.size());
                }
            }
        }
    }

    /** Does nothing because the transient path does not use directory validation. */
    void validate_minimizer_directory_or_error(const char*, std::size_t) const {}

    /** Does nothing because the transient path does not use chunk metadata validation. */
    void validate_minimizer_chunk_metadata_or_error(bioseq::indexing::index_sub_key_t,
                                                    std::size_t,
                                                    std::size_t,
                                                    std::size_t) const {}

    /** Validates minimizer blob header size and optional bucket count. */
    auto validate_minimizer_blob_or_error(const char* blob,
                                          std::size_t bytes,
                                          std::uint32_t expected_bucket_count = 0) const {
        using header_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header;
        if (bytes < sizeof(header_type)) {
            index_error("opportunistic minimizer-sequence: truncated blob");
        }
        header_type header{};
        std::memcpy(&header, blob, sizeof(header));
        if (expected_bucket_count != 0u && header.bucket_count != expected_bucket_count) {
            index_error("opportunistic minimizer-sequence: unexpected bucket count");
        }
        return header;
    }

    /** Raises a PostgreSQL data-corruption error and does not return. */
    [[noreturn]] void index_error(std::string_view message) const {
        bioseq::postgres::support::invoke_pg_as_cpp([message] {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq opportunistic minimizer-sequence index: %.*s",
                            static_cast<int>(message.size()),
                            message.data())));
        });
        pg_unreachable();
    }

  private:
    /** Returns an existing chunk or appends a zero-length placeholder. */
    opportunistic_sequence_chunk& ensure_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                               std::size_t chunk_no) {
        for (opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk;
            }
        }
        chunks.push_back(opportunistic_sequence_chunk{
            .sub_key = sub_key,
            .chunk_no = chunk_no,
            .bytes = {},
        });
        return chunks.back();
    }

    /** Returns a chunk reference or reports a missing chunk. */
    [[nodiscard]] const opportunistic_sequence_chunk&
    chunk_ref(bioseq::indexing::index_sub_key_t sub_key, std::size_t chunk_no) const {
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk;
            }
        }
        const std::string detail = std::format(
            "missing chunk sub_key={} chunk_no={}", static_cast<long long>(sub_key), chunk_no);
        index_error(detail);
    }
};

/** Builds a persist target for a per-fragment minimizer index row. */
template <class p_scanner_pg_context>
[[nodiscard]] bioseq::postgres::scheduler::persist_target make_fragment_index_persist_target(
    const p_scanner_pg_context& ctx,
    const bioseq::storage::sequence_locator& loc,
    std::uint64_t sequence_rowno,
    std::string_view index_name = bioseq::indexing::minimizer_hashmap_fragment_index::index_name) {
    return bioseq::postgres::scheduler::persist_target{
        .db_oid = ctx.db_oid,
        .user_relid = ctx.user_relid,
        .tablespace_oid = ctx.tablespace_oid,
        .main_relfilenode = ctx.main_relfilenode,
        .column_attno = ctx.mmsearch_seq_attno,
        .bucket_idx = static_cast<std::uint8_t>(loc.bucket_index),
        .shard_idx = loc.shard_index,
        .sequence_rowno = sequence_rowno,
        .index_name = index_name,
    };
}

/** True when async recompute may defer minimizer persistence for this scan context. */
template <class p_scanner_pg_context>
[[nodiscard]] bool
can_defer_recompute_minimizer_persist(const p_scanner_pg_context& ctx,
                                      const minimap2_parameters& params) noexcept {
    return ctx.recompute_async_persist_active && ctx.index_async_persist_active &&
           OidIsValid(ctx.user_relid) && params.kmer_size == 15 &&
           params.window_size == 10
           // Existing asynchronous minimizer-fragment/minimizer-sequence payloads
           // have no minimizer-scheme field.
           // Bounded ntHash recompute remains bounded, but persistence must wait for the
           // scheme-tagged READY-generation writer; never relabel it Exact/scalar.
           && pipe::target_index_sketch_backend_from_params(params) !=
                  bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd;
}

// Select the target index sketch backend for recompute from
// the resolved params (Vulkan only when all three high-level gates are on).
[[nodiscard]] inline bioseq::minimizers::minimizer_sketch_backend
target_backend_from_params_for_recompute(const minimap2_parameters& params) noexcept {
    return pipe::target_index_sketch_backend_from_params(params);
}

/** Resolves segment unit ranges for PostgreSQL indexing using the selected sketch backend. */
[[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
pg_segment_unit_ranges(sequence_size_t total_bp,
                       sequence_size_t query_len,
                       std::size_t lpu,
                       const minimap2_parameters& params) {
    const auto backend = ((params.vulkan_enabled && params.vulkan_seed_hits_enabled) ||
                          target_backend_from_params_for_recompute(params) ==
                              bioseq::minimizers::minimizer_sketch_backend::vulkan_segment)
                             ? bioseq::minimizers::minimizer_sketch_backend::vulkan_segment
                             : bioseq::minimizers::minimizer_sketch_backend::scalar;
    return pipe::segment_unit_ranges_for_backend(total_bp, query_len, lpu, backend);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
