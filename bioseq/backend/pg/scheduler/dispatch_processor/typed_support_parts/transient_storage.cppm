/**
 * @file typed_support_parts/transient_storage.cppm
 * @brief Defines transient fragment and minimizer-sequence storage owners.
 */
module;

#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <libassert/assert.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:transient_storage;

import bioseq.postgres.scheduler.dispatch_processor;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.scheduler;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.types.compact_sequence;
import bioseq.instrumentation.clock;
import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.region;
import bioseq.instrumentation.runtime;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

struct opportunistic_index_chunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::vector<char> bytes{};
};

struct opportunistic_fragment_index_storage {
    std::map<bioseq::indexing::index_sub_key_t, std::vector<std::size_t>> plans{};
    std::vector<opportunistic_index_chunk> chunks{};

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

    [[nodiscard]] bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header
    validate_minimizer_blob_or_error(
        const char* p,
        std::size_t bytes,
        std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using header_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_header;
        using bucket_type = bioseq::indexing::minimizer_hashmap_fragment_index::encoded_bucket;
        if (bytes < sizeof(header_type)) {
            index_error("minimizer blob truncated header");
        }
        header_type h{};
        std::memcpy(&h, p, sizeof(header_type));
        if (h.magic != bioseq::indexing::minimizer_hashmap_fragment_index::k_encoded_magic ||
            h.layout_revision !=
                bioseq::indexing::minimizer_hashmap_fragment_index::k_encoded_layout_revision) {
            index_error("minimizer blob magic/layout");
        }
        const std::size_t expect = sizeof(header_type) + sizeof(bucket_type) * h.bucket_count +
                                   sizeof(std::uint64_t) * h.total_positions;
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

    void validate_minimizer_chunk_metadata_or_error(bioseq::indexing::index_sub_key_t sub_key,
                                                    std::size_t chunk_no,
                                                    std::size_t min_bytes,
                                                    std::size_t) const {
        if (chunk_byte_size(sub_key, chunk_no) < min_bytes) {
            index_error("chunk metadata below minimum size");
        }
    }

    [[nodiscard]] std::size_t number_of_chunks(bioseq::indexing::index_sub_key_t sub_key) const {
        std::size_t count = 0;
        for (const opportunistic_index_chunk& chunk : chunks) {
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
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (!have || chunk.sub_key != last) {
                ++count;
                last = chunk.sub_key;
                have = true;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t chunk_byte_size(bioseq::indexing::index_sub_key_t sub_key,
                                              std::size_t chunk_no) const {
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk.bytes.size();
            }
        }
        return 0;
    }

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

    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes_count) {
        const std::size_t n =
            bytes_count == 0 ? 1
                             : (bytes_count + bioseq::indexing::k_max_index_chunk_size_bytes - 1) /
                                   bioseq::indexing::k_max_index_chunk_size_bytes;
        std::vector<std::size_t> plan(n);
        std::size_t remaining = bytes_count;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t bytes =
                std::min(remaining, bioseq::indexing::k_max_index_chunk_size_bytes);
            plan[i] = bytes;
            remaining -= bytes;
        }
        plans[sub_key] = std::move(plan);
    }

    void resize_chunks(bioseq::indexing::index_sub_key_t sub_key,
                       std::span<const std::size_t> per_chunk_bytes) {
        plans[sub_key] = std::vector<std::size_t>(per_chunk_bytes.begin(), per_chunk_bytes.end());
    }

    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

    template <class p_value, class p_function>
    void write_to_each(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        auto it = plans.find(sub_key);
        if (it == plans.end()) {
            index_error("write_to_each without resize");
        }
        const std::vector<std::size_t>& plan = it->second;
        for (std::size_t chunk_no = 0; chunk_no < plan.size();) {
            const std::size_t payload_bytes = plan[chunk_no];
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
        plans.erase(it);
    }

    template <class p_function>
    void read_chunks_in_range(bioseq::indexing::index_sub_key_t lo,
                              bioseq::indexing::index_sub_key_t hi,
                              p_function&& cb) {
        for (const opportunistic_index_chunk& chunk : chunks) {
            if (chunk.sub_key >= lo && chunk.sub_key < hi) {
                cb(chunk.sub_key, chunk.bytes.data(), chunk.bytes.size());
            }
        }
    }

    void for_each_sub_key(auto&& fn) const {
        bioseq::indexing::index_sub_key_t last =
            std::numeric_limits<bioseq::indexing::index_sub_key_t>::min();
        bool have = false;
        for (const opportunistic_index_chunk& chunk : chunks) {
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

    void discard_sub_keys(bioseq::indexing::index_sub_key_t lo,
                          bioseq::indexing::index_sub_key_t hi) {
        chunks.erase(std::remove_if(chunks.begin(),
                                    chunks.end(),
                                    [lo, hi](const opportunistic_index_chunk& chunk) {
                                        return chunk.sub_key >= lo && chunk.sub_key <= hi;
                                    }),
                     chunks.end());
    }

    [[nodiscard]] std::size_t
    spill_sorted_run(std::span<const std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    [[nodiscard]] std::size_t
    spill_sorted_run_owned(std::vector<std::pair<std::uint64_t, std::uint64_t>>) {
        index_error("external sort spill is not supported by opportunistic storage");
    }

    [[nodiscard]] std::size_t
    spill_sorted_run_owned_admitted(std::vector<std::pair<std::uint64_t, std::uint64_t>>,
                                    std::uint64_t,
                                    bioseq::parallelism::memory_permit permit) {
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

// Transient minimizer-sequence storage backing a real no-index per-sequence
// build. Unlike opportunistic_fragment_index_storage (which refuses external-sort
// spill), this delegates spill/merge to a real sorted_pair_run_store so it satisfies
// is_external_sort_spill_storage + is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
// that direct_minimizer_bucket_index<p_sequence> requires. No persisted sidecar rows.
struct opportunistic_sequence_chunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::size_t chunk_no = 0;
    std::vector<char> bytes;
};

struct opportunistic_sequence_index_storage {
    using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;

    std::vector<opportunistic_sequence_chunk> chunks;
    bioseq::storage::indexing::sorted_pair_run_store sorted_runs;
    std::size_t run_buffer_bytes_value = std::size_t{256} << 20;

    [[nodiscard]] std::size_t run_buffer_bytes() const noexcept {
        return run_buffer_bytes_value;
    }

    void clear_entity() {
        chunks.clear();
        sorted_runs.release_runs();
    }

    // Required by is_index_chunked_storage_for<CHUNKED_PER_FRAGMENT>
    // but unused on the transient path.
    void clear(std::string_view) {
        clear_entity();
    }

    void resize_chunks(bioseq::indexing::index_sub_key_t sub_key,
                       std::span<const std::size_t> plan) {
        for (std::size_t chunk_no = 0; chunk_no < plan.size(); ++chunk_no) {
            auto& chunk = ensure_chunk(sub_key, chunk_no);
            chunk.bytes.resize(plan[chunk_no]);
        }
    }

    void resize(bioseq::indexing::index_sub_key_t sub_key, std::size_t bytes) {
        std::array<std::size_t, 1> plan{bytes};
        resize_chunks(sub_key, std::span<const std::size_t>{plan.data(), plan.size()});
    }

    void prefetch_chunk(bioseq::indexing::index_sub_key_t, std::size_t) {}

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

    template <class p_value, class p_function>
    void write_to(bioseq::indexing::index_sub_key_t sub_key, p_function&& writer) {
        write_to_each<p_value>(sub_key, std::forward<p_function>(writer));
    }

    [[nodiscard]] const char* read_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                         std::size_t chunk_no) const {
        return chunk_ref(sub_key, chunk_no).bytes.data();
    }

    template <class p_value>
    [[nodiscard]] const p_value* read_chunk(bioseq::indexing::index_sub_key_t sub_key,
                                            std::size_t chunk_no) const {
        return reinterpret_cast<const p_value*>(chunk_ref(sub_key, chunk_no).bytes.data());
    }

    [[nodiscard]] std::size_t chunk_byte_size(bioseq::indexing::index_sub_key_t sub_key,
                                              std::size_t chunk_no) const {
        return chunk_ref(sub_key, chunk_no).bytes.size();
    }

    [[nodiscard]] std::size_t number_of_chunks(bioseq::indexing::index_sub_key_t sub_key) const {
        std::size_t count = 0;
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key) {
                ++count;
            }
        }
        return count;
    }

    // Reports the distinct sub-keys required by the storage concept.
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        std::size_t count = 0;
        for_each_sub_key([&count](bioseq::indexing::index_sub_key_t) { ++count; });
        return count;
    }

    template <class p_function> void for_each_sub_key(p_function&& fn) const {
        bioseq::indexing::index_sub_key_t last = 0;
        bool have_last = false;
        // chunks are appended in build order; emit each distinct sub_key once.
        std::vector<bioseq::indexing::index_sub_key_t> seen;
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (have_last && chunk.sub_key == last) {
                continue;
            }
            bool already = false;
            for (const auto key : seen) {
                if (key == chunk.sub_key) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                seen.push_back(chunk.sub_key);
                fn(chunk.sub_key);
            }
            last = chunk.sub_key;
            have_last = true;
        }
    }

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

    template <class p_function> void merge_runs(p_function&& on_kmer) {
        sorted_runs.merge_runs(std::forward<p_function>(on_kmer));
    }

    void release_resident_runs_noexcept() noexcept {
        sorted_runs.release_resident_runs_noexcept();
    }

    void release_runs() {
        sorted_runs.release_runs();
    }

    void release_runs_noexcept() noexcept {
        sorted_runs.release_runs_noexcept();
    }

    template <class p_function>
    void read_chunks_in_range(bioseq::indexing::index_sub_key_t first,
                              bioseq::indexing::index_sub_key_t last,
                              p_function&& f) const {
        for (bioseq::indexing::index_sub_key_t sub_key = first; sub_key < last; ++sub_key) {
            for (const opportunistic_sequence_chunk& chunk : chunks) {
                if (chunk.sub_key == sub_key) {
                    f(sub_key, chunk.bytes.data(), chunk.bytes.size());
                }
            }
        }
    }

    void validate_minimizer_directory_or_error(const char*, std::size_t) const {}

    void validate_minimizer_chunk_metadata_or_error(bioseq::indexing::index_sub_key_t,
                                                    std::size_t,
                                                    std::size_t,
                                                    std::size_t) const {}

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

    [[nodiscard]] const opportunistic_sequence_chunk&
    chunk_ref(bioseq::indexing::index_sub_key_t sub_key, std::size_t chunk_no) const {
        for (const opportunistic_sequence_chunk& chunk : chunks) {
            if (chunk.sub_key == sub_key && chunk.chunk_no == chunk_no) {
                return chunk;
            }
        }
        const std::string detail = fmt::format(
            "missing chunk sub_key={} chunk_no={}", static_cast<long long>(sub_key), chunk_no);
        index_error(detail);
    }
};

[[nodiscard]] bioseq::postgres::scheduler::persist_target make_fragment_index_persist_target(
    const batch_processor::scanner_pg_context& ctx,
    const bioseq::storage::sequence_locator& loc,
    std::uint64_t sequence_rowno,
    std::string_view index_name = minimizers_per_fragment_index_name) {
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

[[nodiscard]] bool
can_defer_recompute_minimizer_persist(const batch_processor::scanner_pg_context& ctx,
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

[[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
pg_segment_unit_ranges(sequence_size_t total_bp,
                       sequence_size_t query_len,
                       std::size_t lpu,
                       const minimap2_parameters& params) {
    const auto backend = (pg_vulkan_seed_lookup_selected(params) ||
                          target_backend_from_params_for_recompute(params) ==
                              bioseq::minimizers::minimizer_sketch_backend::vulkan_segment)
                             ? bioseq::minimizers::minimizer_sketch_backend::vulkan_segment
                             : bioseq::minimizers::minimizer_sketch_backend::scalar;
    return pipe::segment_unit_ranges_for_backend(total_bp, query_len, lpu, backend);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
