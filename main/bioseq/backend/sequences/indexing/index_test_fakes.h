#pragma once
//
// DOC0279 §16.1 — in-RAM fake storage backends for the NO_POSTGRES index unit
// tests. Each fake satisfies exactly one of the §6.3 `is_index_chunked_storage_for`
// requires-clauses (and FakeFragmentStorage additionally `is_external_sort_spill_storage`),
// so the conformance static_asserts at the bottom of this header force the member
// signatures to stay 1:1 with the concepts in index_chunked_storage.cppm.
//
// The validators (validate_minimizer_*_or_error / index_error) mirror the pg-side
// IndexStorageFragmentPg surface (index_chunked_storage_pg.cppm:206-230) so the
// algorithm modules (assemble_segment / assemble_for_query) can call them duck-typed
// on the fake exactly as they do on the real pg storage.
//
// This is a shared HEADER, NOT a test (no TEST_CASE). It is #included by the
// *_test.cpp files in this directory.
//
// IMPORTANT: this header deliberately carries NO `import` declarations (the repo
// convention is that .h files never import modules — modules are imported once at
// the top of each consuming .cpp). Every includer MUST therefore have already
// imported, BEFORE the `#include "index_test_fakes.h"`:
//   import bioseq_indexing.type;       // SequenceIndexType, index_sub_key_t, DIRECTORY_SUB_KEY, TEMP_RUN_BASE, MAX_INDEX_CHUNK_SIZE_BYTES
//   import bioseq_indexing.storage;    // is_index_chunked_storage_for, is_external_sort_spill_storage
//   import bioseq_indexing.minimizer_hashmap;  // MinimizerHashmapFragmentIndex, index_corruption_panic
// (the §16.1 preamble already lists all three.)

#include "common.h"            // DEBUG_ASSERT (unused here but harmless), parity with modules

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace bioseq::indexing::test {

// =====================================================================
// FakeFragmentStorage — CHUNKED_PER_FRAGMENT + is_external_sort_spill_storage
// =====================================================================
//
// 2-D (sub_key, chunk_no) chunk space backed by std::map. `kMaxChunk` is the
// per-chunk payload cap (templated so the chunk-io test can use a tiny value).
template <std::size_t kMaxChunk = bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES>
struct FakeFragmentStorage {
    // (sub_key, chunk_no) -> raw payload bytes.
    std::map<std::pair<std::int64_t, std::int64_t>, std::vector<char>> chunks_;
    // sub_key -> per-chunk byte plan (set by resize / resize_chunks; consumed by write_to_each).
    std::map<std::int64_t, std::vector<std::size_t>> planned_;
    // Reusable read buffer so read_chunk<T> can return a stable pointer.
    mutable std::vector<std::int64_t> read_buf_;

    // ---- error / validation surface (duck-typed; mirrors the pg storage) ----
    [[noreturn]] void index_error(std::string_view m) const { bioseq::indexing::index_corruption_panic(m); }

    [[nodiscard]] MinimizerHashmapFragmentIndex::SerializedHeader validate_minimizer_blob_or_error(
            const char* p, std::size_t bytes,
            std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using H = MinimizerHashmapFragmentIndex::SerializedHeader;
        using B = MinimizerHashmapFragmentIndex::SerializedBucket;
        if (bytes < sizeof(H)) index_error("minimizer blob truncated header");
        H h{}; std::memcpy(&h, p, sizeof(H));
        if (h.magic != MinimizerHashmapFragmentIndex::kSerializeMagic
            || h.version != MinimizerHashmapFragmentIndex::kSerializeVersion)
            index_error("minimizer blob magic/version");
        const std::size_t expect = sizeof(H) + sizeof(B) * h.bucket_count
                                  + sizeof(std::uint64_t) * h.total_positions;
        if (bytes != expect) index_error("minimizer blob size mismatch");
        if (expected_bucket_count && h.bucket_count != *expected_bucket_count)
            index_error("minimizer blob bucket_count mismatch");
        return h;
    }
    void validate_minimizer_directory_or_error(const char* p, std::size_t bytes) const {
        if (bytes < 32) index_error("directory truncated");
        std::uint32_t magic{}, version{};
        std::memcpy(&magic, p, 4); std::memcpy(&version, p + 4, 4);
        if (magic != 0xB1000002u || version != 1u) index_error("directory magic/version");
    }
    void validate_minimizer_chunk_metadata_or_error(index_sub_key_t sk, std::size_t chunk_no,
            std::size_t min_bytes, std::size_t /*alignment*/) const {
        if (chunk_byte_size(sk, chunk_no) < min_bytes) index_error("chunk metadata below minimum size");
    }

    // ---- per-fragment chunk surface (§6.3 CHUNKED_PER_FRAGMENT) ----
    [[nodiscard]] std::size_t number_of_sub_keys() const {
        // distinct sub_keys in [0, TEMP_RUN_BASE) that have chunk_no==0 OR any chunk.
        std::size_t count = 0;
        std::int64_t last = 0; bool have_last = false;
        for (const auto& kv : chunks_) {
            const std::int64_t sk = kv.first.first;
            if (sk < 0 || sk >= bioseq::indexing::TEMP_RUN_BASE) continue;
            if (!have_last || sk != last) { ++count; last = sk; have_last = true; }
        }
        return count;
    }

    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const {
        std::size_t count = 0;
        for (std::int64_t c = 0;; ++c) {
            if (chunks_.find({sub_key, c}) == chunks_.end()) break;
            ++count;
        }
        return count;
    }

    [[nodiscard]] std::size_t chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const {
        auto it = chunks_.find({sub_key, (std::int64_t)chunk_no});
        return it == chunks_.end() ? std::size_t{0} : it->second.size();
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        auto it = chunks_.find({sub_key, (std::int64_t)chunk_no});
        if (it == chunks_.end()) index_error("FakeFragmentStorage::read_chunk: missing chunk");
        const std::size_t bytes = it->second.size();
        const std::size_t units = (bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        read_buf_.assign(units == 0 ? 1 : units, std::int64_t{0});
        if (bytes > 0) std::memcpy(read_buf_.data(), it->second.data(), bytes);
        return reinterpret_cast<const T*>(read_buf_.data());
    }

    void resize(index_sub_key_t sub_key, std::size_t bytes) {
        const std::size_t n_chunks = bytes == 0 ? std::size_t{1}
                                   : (bytes + kMaxChunk - 1) / kMaxChunk;
        std::vector<std::size_t> plan(n_chunks == 0 ? 1 : n_chunks);
        for (std::size_t c = 0; c + 1 < plan.size(); ++c) plan[c] = kMaxChunk;
        plan.back() = bytes - (plan.size() - 1) * kMaxChunk;
        planned_[sub_key] = std::move(plan);
    }

    void resize_chunks(index_sub_key_t sub_key, std::span<const std::size_t> per_chunk_bytes) {
        planned_[sub_key].assign(per_chunk_bytes.begin(), per_chunk_bytes.end());
    }

    void prefetch_chunk(index_sub_key_t, std::size_t) {}

    template <class F>
    void read_chunks_in_range(index_sub_key_t lo, index_sub_key_t hi, F&& cb) {
        if (hi <= lo) return;   // HALF-OPEN [lo, hi)
        for (index_sub_key_t sub_key = lo; sub_key < hi; ++sub_key) {
            const std::size_t nc = number_of_chunks(sub_key);
            for (std::size_t chunk_no = 0; chunk_no < nc; ++chunk_no) {
                auto& blob = chunks_[{sub_key, (std::int64_t)chunk_no}];
                cb(sub_key, blob.data(), blob.size());
            }
        }
    }

    template <class F>
    void for_each_sub_key(F&& fn) const {
        std::int64_t last = 0; bool have_last = false;
        for (const auto& kv : chunks_) {
            const std::int64_t sk = kv.first.first;
            if (sk < 0 || sk >= bioseq::indexing::TEMP_RUN_BASE) continue;
            if (!have_last || sk != last) { fn(sk); last = sk; have_last = true; }
        }
    }

    template <class T, class F>
    void write_to_each(index_sub_key_t sub_key, F&& writer) {
        auto pit = planned_.find(sub_key);
        if (pit == planned_.end()) index_error("FakeFragmentStorage::write_to_each: sub_key not planned");
        const std::vector<std::size_t>& plan = pit->second;
        std::size_t chunk_no = 0;
        while (chunk_no < plan.size()) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (payload_bytes % sizeof(T) != 0)
                index_error("FakeFragmentStorage::write_to_each: chunk byte plan not T-aligned");
            std::vector<char> buf(payload_bytes, char{0});
            const std::size_t next = writer((std::size_t)chunk_no,
                                            reinterpret_cast<T*>(buf.data()),
                                            payload_bytes / sizeof(T));
            chunks_[{sub_key, (std::int64_t)chunk_no}] = std::move(buf);
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
    }

    void clear_entity() { chunks_.clear(); planned_.clear(); }

    void discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi) {
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            if (it->first.first >= lo && it->first.first <= hi) it = chunks_.erase(it);
            else ++it;
        }
        for (auto it = planned_.begin(); it != planned_.end();) {
            if (it->first >= lo && it->first <= hi) it = planned_.erase(it);
            else ++it;
        }
    }

    void clear(std::string_view) { clear_entity(); }

    // ---- external-sort spill surface (§6.3 is_external_sort_spill_storage) ----
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> runs_;
    std::size_t run_buffer_bytes_ = std::size_t{64} << 20;

    std::size_t spill_sorted_run(std::span<const std::pair<std::uint64_t, std::uint64_t>> s) {
        runs_.push_back(std::vector<std::pair<std::uint64_t, std::uint64_t>>(s.begin(), s.end()));
        return runs_.size() - 1;
    }

    struct Cursor {
        const std::vector<std::uint64_t>* v;
        [[nodiscard]] std::uint64_t total_count() const { return v->size(); }
        template <class G>
        void for_each_batch(G&& g) const {
            if (!v->empty()) g(std::span<const std::uint64_t>(v->data(), v->size()));
        }
    };

    template <class F>
    void merge_runs(F&& on_kmer) {
        // IN-RAM k-way merge: concatenate every spilled run, sort by (kmer, encoded),
        // then emit each distinct kmer in ASCENDING kmer order with its UNIQUE encoded
        // positions (ascending).
        std::vector<std::pair<std::uint64_t, std::uint64_t>> all;
        for (const auto& run : runs_) all.insert(all.end(), run.begin(), run.end());
        std::sort(all.begin(), all.end());
        std::size_t i = 0;
        const std::size_t n = all.size();
        std::vector<std::uint64_t> positions;
        while (i < n) {
            const std::uint64_t kmer = all[i].first;
            positions.clear();
            std::size_t j = i;
            while (j < n && all[j].first == kmer) {
                if (positions.empty() || positions.back() != all[j].second)
                    positions.push_back(all[j].second);   // UNIQUE encoded positions
                ++j;
            }
            Cursor cur{&positions};
            on_kmer(kmer, cur);
            i = j;
        }
    }

    void release_runs() { runs_.clear(); }
    [[nodiscard]] std::size_t run_buffer_bytes() const { return run_buffer_bytes_; }
};

// =====================================================================
// FakeSequenceStorage — INLINE_PER_SEQUENCE (exactly one chunk)
// =====================================================================
struct FakeSequenceStorage {
    std::vector<char> chunk_;
    std::size_t planned_bytes_ = 0;
    bool has_plan_ = false;
    mutable std::vector<std::int64_t> read_buf_;

    [[noreturn]] void index_error(std::string_view m) const { bioseq::indexing::index_corruption_panic(m); }

    [[nodiscard]] std::size_t number_of_chunks() const { return chunk_.empty() ? std::size_t{0} : std::size_t{1}; }
    [[nodiscard]] std::size_t chunk_byte_size() const { return chunk_.size(); }

    template <class T>
    [[nodiscard]] const T* read_chunk() {
        const std::size_t bytes = chunk_.size();
        const std::size_t units = (bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        read_buf_.assign(units == 0 ? 1 : units, std::int64_t{0});
        if (bytes > 0) std::memcpy(read_buf_.data(), chunk_.data(), bytes);
        return reinterpret_cast<const T*>(read_buf_.data());
    }

    void resize(std::size_t bytes) {
        if (bytes > bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES)
            index_error("FakeSequenceStorage::resize: inline chunk exceeds MAX_INDEX_CHUNK_SIZE_BYTES");
        planned_bytes_ = bytes;
        has_plan_ = true;
    }

    template <class T, class F>
    void write_to_each(F&& w) {
        if (!has_plan_) index_error("FakeSequenceStorage::write_to_each: no plan (call resize first)");
        if (planned_bytes_ % sizeof(T) != 0)
            index_error("FakeSequenceStorage::write_to_each: planned bytes not T-aligned");
        std::vector<char> buf(planned_bytes_, char{0});
        const std::size_t next = w((std::size_t)0, reinterpret_cast<T*>(buf.data()), planned_bytes_ / sizeof(T));
        if (next != 0) index_error("FakeSequenceStorage::write_to_each: inline storage has exactly one chunk");
        chunk_ = std::move(buf);
    }

    void prefetch_chunk() {}
    void clear_entity() { chunk_.clear(); planned_bytes_ = 0; has_plan_ = false; }
    void clear(std::string_view) { clear_entity(); }
};

// =====================================================================
// FakeTableStorage — CHUNKED_PER_TABLE (flat chunk space + write_to + for_each_sequence)
// =====================================================================
struct FakeTableStorage {
    std::map<std::size_t, std::vector<char>> chunks_;
    std::vector<std::size_t> planned_;
    std::vector<int> seq_rownos_;          // rownos fed to for_each_sequence
    mutable std::vector<std::int64_t> read_buf_;

    [[noreturn]] void index_error(std::string_view m) const { bioseq::indexing::index_corruption_panic(m); }

    [[nodiscard]] std::size_t number_of_chunks() const { return chunks_.size(); }
    [[nodiscard]] std::size_t chunk_byte_size(std::size_t chunk_no) const {
        auto it = chunks_.find(chunk_no);
        return it == chunks_.end() ? std::size_t{0} : it->second.size();
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(std::size_t chunk_no) {
        auto it = chunks_.find(chunk_no);
        if (it == chunks_.end()) index_error("FakeTableStorage::read_chunk: missing chunk");
        const std::size_t bytes = it->second.size();
        const std::size_t units = (bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        read_buf_.assign(units == 0 ? 1 : units, std::int64_t{0});
        if (bytes > 0) std::memcpy(read_buf_.data(), it->second.data(), bytes);
        return reinterpret_cast<const T*>(read_buf_.data());
    }

    void resize(std::size_t bytes) {
        const std::size_t n_chunks = bytes == 0 ? std::size_t{1}
                                   : (bytes + bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES - 1)
                                       / bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
        planned_.assign(n_chunks == 0 ? 1 : n_chunks, std::size_t{0});
        for (std::size_t c = 0; c + 1 < planned_.size(); ++c) planned_[c] = bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
        planned_.back() = bytes - (planned_.size() - 1) * bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
    }

    void prefetch_chunk(std::size_t) {}

    template <class T, class F>
    void write_to_each(F&& writer) {
        for (std::size_t chunk_no = 0; chunk_no < planned_.size(); ++chunk_no) {
            const std::size_t payload_bytes = planned_[chunk_no];
            if (payload_bytes % sizeof(T) != 0) index_error("FakeTableStorage::write_to_each: chunk not T-aligned");
            std::vector<char> buf(payload_bytes, char{0});
            (void)writer((std::size_t)chunk_no, reinterpret_cast<T*>(buf.data()), payload_bytes / sizeof(T));
            chunks_[chunk_no] = std::move(buf);
        }
    }

    template <class T, class F>
    void write_to(std::size_t chunk_no, F&& writer) {
        if (chunk_no >= planned_.size()) index_error("FakeTableStorage::write_to: chunk_no past plan (call resize first)");
        const std::size_t payload_bytes = planned_[chunk_no];
        if (payload_bytes % sizeof(T) != 0) index_error("FakeTableStorage::write_to: chunk not T-aligned");
        std::vector<char> buf(payload_bytes, char{0});
        (void)writer((std::size_t)chunk_no, reinterpret_cast<T*>(buf.data()), payload_bytes / sizeof(T));
        chunks_[chunk_no] = std::move(buf);
    }

    template <class F>
    void for_each_sequence(F&& loader) {
        // The fake has no real lazy sequence; pass a placeholder int that the
        // table-index loaders (which key off rowno/total) ignore.
        for (std::size_t i = 0; i < seq_rownos_.size(); ++i) {
            int dummy = 0;
            loader(dummy, (std::size_t)seq_rownos_[i], (std::size_t)seq_rownos_.size());
        }
    }

    void clear(std::string_view) { chunks_.clear(); planned_.clear(); }
};

// =====================================================================
// §6.3 conformance — the static_asserts force the member signatures above
// to stay 1:1 with the concepts. A drift makes THIS header fail to compile.
// =====================================================================
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  FakeFragmentStorage<>, bioseq::indexing::SequenceIndexType::CHUNKED_PER_FRAGMENT>);
static_assert(bioseq::indexing::is_external_sort_spill_storage<FakeFragmentStorage<>>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  FakeSequenceStorage, bioseq::indexing::SequenceIndexType::INLINE_PER_SEQUENCE>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  FakeTableStorage, bioseq::indexing::SequenceIndexType::CHUNKED_PER_TABLE>);

}  // namespace bioseq::indexing::test
