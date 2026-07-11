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
//   import bioseq_parallelism.memory_admission; // MemoryPermit for admitted sorted runs
// (the §16.1 preamble already lists all three.)

#include "common.h"            // DEBUG_ASSERT (unused here but harmless), parity with modules

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace bioseq::indexing::test {

/// Test-visible hard error raised by the fake qualified bulk-reader contract.
class FakeQualifiedChunkReadError final : public std::runtime_error {
public:
    explicit FakeQualifiedChunkReadError(const std::string& message)
        : std::runtime_error(message) {}
};

// =====================================================================
// FakeFragmentStorage — CHUNKED_PER_FRAGMENT + is_external_sort_spill_storage
// =====================================================================
//
// Qualified (generation, position_segment, sub_key, chunk_no) chunk space backed
// by std::map. `kMaxChunk` is the per-chunk payload cap (templated so the chunk-io
// test can use a tiny value). Legacy APIs operate only on the (0,0) qualifier.
template <std::size_t kMaxChunk = bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES>
struct FakeFragmentStorage {
    using qualified_chunk_key_t =
        std::tuple<std::uint64_t, std::int32_t, index_sub_key_t, std::uint32_t>;

    struct QualifiedChunkRow {
        std::uint32_t chunk_bytes = 0;
        std::uint64_t checksum = 0;
        std::vector<char> blob;
    };

    struct CacheTouchProbe {
        std::uint64_t lookups = 0;
        std::uint64_t populations = 0;
    };

    // (generation, position_segment, sub_key, chunk_no) -> persisted row.
    // Legacy operations use the isolated (generation=0, position_segment=0) plane.
    std::map<qualified_chunk_key_t, QualifiedChunkRow> chunks_;
    // Immutable directory knowledge used to authorize only predeclared gap rows.
    std::set<qualified_chunk_key_t> directory_declared_chunk_keys_;
    CacheTouchProbe cache_touch_probe_;
    // sub_key -> per-chunk byte plan (set by resize / resize_chunks; consumed by write_to_each).
    std::map<std::int64_t, std::vector<std::size_t>> planned_;
    // Reusable read buffer so read_chunk<T> can return a stable pointer.
    mutable std::vector<std::int64_t> read_buf_;

    [[nodiscard]] static constexpr qualified_chunk_key_t qualified_key(
            const IndexChunkAddress& address) noexcept {
        return {address.generation_id, address.position_segment,
                address.sub_key, address.chunk_no};
    }

    [[nodiscard]] static constexpr qualified_chunk_key_t legacy_key(
            index_sub_key_t sub_key,
            std::size_t chunk_no) noexcept {
        return {0, 0, sub_key, static_cast<std::uint32_t>(chunk_no)};
    }

    [[nodiscard]] static std::uint64_t payload_row_checksum(
            const IndexChunkAddress& address,
            std::uint32_t chunk_bytes,
            std::span<const char> payload) noexcept {
        return bioseq::indexing::crc64_index_chunk_row(
            address.generation_id, address.position_segment, address.sub_key,
            address.chunk_no, chunk_bytes, std::as_bytes(payload));
    }

    [[nodiscard]] static QualifiedChunkRow make_chunk_row(
            const IndexChunkAddress& address,
            std::vector<char> blob) {
        if (blob.size() > static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw FakeQualifiedChunkReadError("fake chunk exceeds uint32 metadata");
        }
        QualifiedChunkRow row;
        row.chunk_bytes = static_cast<std::uint32_t>(blob.size());
        row.checksum = payload_row_checksum(
            address, row.chunk_bytes, std::span<const char>{blob});
        row.blob = std::move(blob);
        return row;
    }

    [[noreturn]] static void qualified_read_error(std::string_view message) {
        throw FakeQualifiedChunkReadError(std::string{message});
    }

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
        if (bytes < 40) index_error("directory truncated");
        std::uint32_t magic{}, version{};
        std::memcpy(&magic, p, 4); std::memcpy(&version, p + 4, 4);
        if (magic != 0xB1000002u || version != 2u) index_error("directory magic/version");
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
            if (std::get<0>(kv.first) != 0 || std::get<1>(kv.first) != 0) continue;
            const std::int64_t sk = std::get<2>(kv.first);
            if (sk < 0 || sk >= bioseq::indexing::TEMP_RUN_BASE) continue;
            if (!have_last || sk != last) { ++count; last = sk; have_last = true; }
        }
        return count;
    }

    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const {
        std::size_t count = 0;
        for (std::size_t c = 0;; ++c) {
            if (chunks_.find(legacy_key(sub_key, c)) == chunks_.end()) break;
            ++count;
        }
        return count;
    }

    [[nodiscard]] std::size_t chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const {
        auto it = chunks_.find(legacy_key(sub_key, chunk_no));
        return it == chunks_.end() ? std::size_t{0} : it->second.blob.size();
    }

    template <class T>
    [[nodiscard]] const T* read_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
        auto it = chunks_.find(legacy_key(sub_key, chunk_no));
        if (it == chunks_.end()) index_error("FakeFragmentStorage::read_chunk: missing chunk");
        const std::size_t bytes = it->second.blob.size();
        const std::size_t units = (bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        read_buf_.assign(units == 0 ? 1 : units, std::int64_t{0});
        if (bytes > 0) std::memcpy(read_buf_.data(), it->second.blob.data(), bytes);
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
                auto& blob = chunks_[legacy_key(sub_key, chunk_no)].blob;
                cb(sub_key, blob.data(), blob.size());
            }
        }
    }

    void store_qualified_chunk_for_test(
            const IndexChunkAddress& address,
            std::span<const char> payload) {
        std::vector<char> blob(payload.begin(), payload.end());
        chunks_[qualified_key(address)] = make_chunk_row(address, std::move(blob));
        directory_declared_chunk_keys_.insert(qualified_key(address));
    }

    void store_undeclared_qualified_chunk_for_test(
            const IndexChunkAddress& address,
            std::span<const char> payload) {
        std::vector<char> blob(payload.begin(), payload.end());
        chunks_[qualified_key(address)] = make_chunk_row(address, std::move(blob));
    }

    [[nodiscard]] QualifiedChunkRow& qualified_chunk_row_for_test(
            const IndexChunkAddress& address) {
        auto it = chunks_.find(qualified_key(address));
        if (it == chunks_.end()) {
            qualified_read_error("qualified test chunk is missing");
        }
        return it->second;
    }

    void erase_qualified_chunk_for_test(const IndexChunkAddress& address) {
        chunks_.erase(qualified_key(address));
    }

    void reset_cache_touch_probe_for_test() noexcept {
        cache_touch_probe_ = {};
    }

    [[nodiscard]] CacheTouchProbe cache_touch_probe_for_test() const noexcept {
        return cache_touch_probe_;
    }

    // Streaming contract: each row is metadata/checksum-valid before its callback,
    // but aggregate extent bytes/CRC are known only after its final row. A late
    // aggregate error does not roll callbacks back; the consumer must keep the
    // unfinished object unpublished and discard it.
    template <index_chunk_reader_fn F>
    [[nodiscard]] IndexChunkReadStats read_chunks_in_range(
            std::span<const IndexSubKeyRange> sorted_nonoverlapping_ranges,
            std::span<const IndexSubKeyRequest> sorted_unique_requests,
            IndexChunkCachePolicy cache_policy,
            F&& sink) {
        using subkey_identity_t =
            std::tuple<std::uint64_t, std::int32_t, index_sub_key_t>;

        struct RequestState {
            IndexSubKeyRequest request;
            std::uint64_t end_chunk_no = 0;
            std::uint64_t next_chunk_no = 0;
            std::uint64_t total_bytes = 0;
            Crc64Ecma182 checksum;
        };

        const auto checked_add = [](std::uint64_t& value,
                                    std::uint64_t increment,
                                    std::string_view what) {
            if (increment > (std::numeric_limits<std::uint64_t>::max)() - value) {
                qualified_read_error(what);
            }
            value += increment;
        };
        const auto validate_qualifier = [](std::uint64_t generation_id,
                                           std::int32_t position_segment) {
            if (generation_id == 0 || !sql_bigint_can_represent(generation_id)) {
                qualified_read_error("qualified read generation does not fit positive SQL bigint");
            }
            if (position_segment < 0) {
                qualified_read_error("qualified read position segment is negative");
            }
        };

        switch (cache_policy) {
            case IndexChunkCachePolicy::populate_residency:
            case IndexChunkCachePolicy::bypass_one_shot:
                break;
            default:
                qualified_read_error("qualified read cache policy is invalid");
        }
        if (sorted_nonoverlapping_ranges.empty()
            != sorted_unique_requests.empty()) {
            qualified_read_error(
                "qualified read ranges and requests must both be empty or nonempty");
        }

        for (std::size_t i = 0; i < sorted_nonoverlapping_ranges.size(); ++i) {
            const IndexSubKeyRange& range = sorted_nonoverlapping_ranges[i];
            validate_qualifier(range.generation_id, range.position_segment);
            if (range.begin >= range.end) {
                qualified_read_error("qualified read range is empty or reversed");
            }
            if (i == 0) continue;
            const IndexSubKeyRange& previous = sorted_nonoverlapping_ranges[i - 1];
            const auto previous_key = std::tuple{
                previous.generation_id, previous.position_segment, previous.begin};
            const auto current_key = std::tuple{
                range.generation_id, range.position_segment, range.begin};
            if (!(previous_key < current_key)) {
                qualified_read_error("qualified read ranges are not strictly sorted");
            }
            if (previous.generation_id == range.generation_id
                && previous.position_segment == range.position_segment
                && range.begin < previous.end) {
                qualified_read_error("qualified read ranges overlap");
            }
        }

        std::vector<RequestState> states;
        states.reserve(sorted_unique_requests.size());
        std::set<subkey_identity_t> wanted_subkeys;
        std::uint64_t wanted_chunks = 0;
        constexpr std::uint64_t max_sql_chunk_no = static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)());

        for (std::size_t i = 0; i < sorted_unique_requests.size(); ++i) {
            const IndexSubKeyRequest& request = sorted_unique_requests[i];
            validate_qualifier(request.generation_id, request.position_segment);
            if (request.expected_chunk_count == 0) {
                qualified_read_error("qualified read extent has zero chunks");
            }
            const std::uint64_t end_chunk_no =
                static_cast<std::uint64_t>(request.first_chunk_no)
                + request.expected_chunk_count;
            if (request.first_chunk_no > max_sql_chunk_no
                || end_chunk_no == 0
                || end_chunk_no - 1u > max_sql_chunk_no) {
                qualified_read_error("qualified read chunk interval exceeds SQL integer");
            }

            if (i != 0) {
                const IndexSubKeyRequest& previous = sorted_unique_requests[i - 1];
                const auto previous_key = std::tuple{
                    previous.generation_id, previous.position_segment,
                    previous.sub_key, previous.first_chunk_no};
                const auto current_key = std::tuple{
                    request.generation_id, request.position_segment,
                    request.sub_key, request.first_chunk_no};
                if (!(previous_key < current_key)) {
                    qualified_read_error("qualified read requests are not sorted and unique");
                }
                const std::uint64_t previous_end =
                    static_cast<std::uint64_t>(previous.first_chunk_no)
                    + previous.expected_chunk_count;
                if (previous.generation_id == request.generation_id
                    && previous.position_segment == request.position_segment
                    && previous.sub_key == request.sub_key
                    && request.first_chunk_no < previous_end) {
                    qualified_read_error("qualified read request intervals overlap");
                }
            }

            bool covered = false;
            for (const IndexSubKeyRange& range : sorted_nonoverlapping_ranges) {
                if (range.generation_id == request.generation_id
                    && range.position_segment == request.position_segment
                    && range.begin <= request.sub_key
                    && request.sub_key < range.end) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                qualified_read_error("qualified read request is outside every range");
            }

            wanted_subkeys.emplace(
                request.generation_id, request.position_segment, request.sub_key);
            checked_add(wanted_chunks, request.expected_chunk_count,
                        "qualified read wanted chunk count overflow");
            states.push_back(RequestState{
                .request = request,
                .end_chunk_no = end_chunk_no,
                .next_chunk_no = request.first_chunk_no,
            });
        }

        for (const IndexSubKeyRange& range : sorted_nonoverlapping_ranges) {
            bool covers_request = false;
            for (const IndexSubKeyRequest& request : sorted_unique_requests) {
                if (range.generation_id == request.generation_id
                    && range.position_segment == request.position_segment
                    && range.begin <= request.sub_key
                    && request.sub_key < range.end) {
                    covers_request = true;
                    break;
                }
            }
            if (!covers_request) {
                qualified_read_error("qualified read range covers no exact request");
            }
        }

        IndexChunkReadStats stats;
        if (!sorted_nonoverlapping_ranges.empty()) {
            stats.relation_opens = 1;
            stats.index_scans = 1;
        }
        stats.requested_ranges = sorted_nonoverlapping_ranges.size();
        stats.wanted_subkeys = wanted_subkeys.size();
        stats.wanted_extents = sorted_unique_requests.size();
        stats.wanted_chunks = wanted_chunks;

        for (const qualified_chunk_key_t& declared : directory_declared_chunk_keys_) {
            const auto& [generation_id, position_segment, sub_key, chunk_no] = declared;
            (void)chunk_no;
            if (wanted_subkeys.contains(subkey_identity_t{
                    generation_id, position_segment, sub_key})) {
                continue;
            }
            for (const IndexSubKeyRange& range : sorted_nonoverlapping_ranges) {
                if (range.generation_id == generation_id
                    && range.position_segment == position_segment
                    && range.begin <= sub_key
                    && sub_key < range.end) {
                    checked_add(stats.skip_budget_rows, 1,
                                "qualified read skip-budget count overflow");
                    break;
                }
            }
        }

        for (const IndexSubKeyRange& range : sorted_nonoverlapping_ranges) {
            auto it = chunks_.lower_bound(qualified_chunk_key_t{
                range.generation_id, range.position_segment, range.begin, 0});
            while (it != chunks_.end()) {
                const auto& [generation_id, position_segment, sub_key, chunk_no] = it->first;
                if (generation_id != range.generation_id
                    || position_segment != range.position_segment
                    || sub_key >= range.end) {
                    break;
                }

                checked_add(stats.heap_rows_read, 1,
                            "qualified read heap-row count overflow");
                RequestState* matched = nullptr;
                for (RequestState& state : states) {
                    if (state.request.generation_id == generation_id
                        && state.request.position_segment == position_segment
                        && state.request.sub_key == sub_key
                        && chunk_no >= state.request.first_chunk_no
                        && static_cast<std::uint64_t>(chunk_no) < state.end_chunk_no) {
                        if (matched != nullptr) {
                            qualified_read_error("qualified chunk matched overlapping extents");
                        }
                        matched = &state;
                    }
                }

                if (matched == nullptr) {
                    if (wanted_subkeys.contains(subkey_identity_t{
                            generation_id, position_segment, sub_key})) {
                        qualified_read_error(
                            "qualified chunk number is outside every requested interval");
                    }
                    if (!directory_declared_chunk_keys_.contains(it->first)) {
                        qualified_read_error(
                            "qualified read encountered an undeclared gap row");
                    }
                    checked_add(stats.skipped_rows, 1,
                                "qualified read skipped-row count overflow");
                    ++it;
                    continue;
                }

                if (matched->next_chunk_no != chunk_no) {
                    qualified_read_error("qualified read extent is missing or non-contiguous");
                }
                const QualifiedChunkRow& row = it->second;
                if (row.chunk_bytes > max_sql_chunk_no
                    || row.chunk_bytes != row.blob.size()) {
                    qualified_read_error("qualified read row byte-count metadata disagrees");
                }
                const std::span<const char> payload{row.blob};
                const IndexChunkAddress address{
                    .generation_id = generation_id,
                    .position_segment = position_segment,
                    .sub_key = sub_key,
                    .chunk_no = chunk_no,
                };
                if (row.checksum != payload_row_checksum(
                        address, row.chunk_bytes, payload)) {
                    qualified_read_error("qualified read row checksum disagrees");
                }
                checked_add(matched->total_bytes, row.blob.size(),
                            "qualified read extent byte count overflow");
                matched->checksum.update(std::as_bytes(payload));

                if (cache_policy == IndexChunkCachePolicy::populate_residency) {
                    checked_add(cache_touch_probe_.lookups, 1,
                                "qualified fake cache lookup count overflow");
                    checked_add(cache_touch_probe_.populations, 1,
                                "qualified fake cache population count overflow");
                }

                sink(address, row.blob.data(), row.blob.size());
                ++matched->next_chunk_no;
                checked_add(stats.delivered_chunks, 1,
                            "qualified read delivered-chunk count overflow");
                checked_add(stats.delivered_bytes, row.blob.size(),
                            "qualified read delivered-byte count overflow");
                ++it;
            }
        }

        for (const RequestState& state : states) {
            if (state.next_chunk_no != state.end_chunk_no) {
                qualified_read_error("qualified read extent has missing trailing chunks");
            }
            if (state.total_bytes != state.request.expected_total_bytes) {
                qualified_read_error("qualified read extent total bytes disagree");
            }
            if (state.checksum.checksum() != state.request.expected_checksum) {
                qualified_read_error("qualified read extent checksum disagrees");
            }
        }

        stats.read_subkeys = wanted_subkeys.size();
        stats.read_extents = sorted_unique_requests.size();
        if (stats.delivered_chunks != stats.wanted_chunks
            || stats.heap_rows_read != stats.delivered_chunks + stats.skipped_rows
            || stats.skipped_rows > stats.skip_budget_rows) {
            qualified_read_error("qualified read proof-counter equation failed");
        }
        return stats;
    }

    template <class F>
    void for_each_sub_key(F&& fn) const {
        std::int64_t last = 0; bool have_last = false;
        for (const auto& kv : chunks_) {
            if (std::get<0>(kv.first) != 0 || std::get<1>(kv.first) != 0) continue;
            const std::int64_t sk = std::get<2>(kv.first);
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
            const IndexChunkAddress address{
                .generation_id = 0,
                .position_segment = 0,
                .sub_key = sub_key,
                .chunk_no = static_cast<std::uint32_t>(chunk_no),
            };
            chunks_[legacy_key(sub_key, chunk_no)] =
                make_chunk_row(address, std::move(buf));
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
    }

    void clear_entity() {
        chunks_.clear();
        directory_declared_chunk_keys_.clear();
        cache_touch_probe_ = {};
        planned_.clear();
    }

    void discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi) {
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            const index_sub_key_t sub_key = std::get<2>(it->first);
            if (sub_key >= lo && sub_key <= hi) it = chunks_.erase(it);
            else ++it;
        }
        for (auto it = planned_.begin(); it != planned_.end();) {
            if (it->first >= lo && it->first <= hi) it = planned_.erase(it);
            else ++it;
        }
        for (auto it = directory_declared_chunk_keys_.begin();
             it != directory_declared_chunk_keys_.end();) {
            const index_sub_key_t sub_key = std::get<2>(*it);
            if (sub_key >= lo && sub_key <= hi) {
                it = directory_declared_chunk_keys_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear(std::string_view) { clear_entity(); }

    // ---- external-sort spill surface (§6.3 is_external_sort_spill_storage) ----
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> runs_;
    std::map<
        std::uint32_t,
        std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>>>
        segment_runs_;
    std::size_t run_buffer_bytes_ = std::size_t{64} << 20;
    std::uint64_t segment_spill_bytes_ = 0;
    std::uint64_t segment_merge_calls_ = 0;
    std::uint64_t segment_merge_run_visits_ = 0;
    std::uint64_t segment_merge_bytes_visited_ = 0;
    std::uint64_t segment_reroute_calls_ = 0;
    std::uint64_t segment_reroute_run_visits_ = 0;
    std::uint64_t segment_reroute_bytes_visited_ = 0;

    std::size_t spill_sorted_run(std::span<const std::pair<std::uint64_t, std::uint64_t>> s) {
        runs_.push_back(std::vector<std::pair<std::uint64_t, std::uint64_t>>(s.begin(), s.end()));
        return runs_.size() - 1;
    }

    std::size_t spill_sorted_run_owned(
            std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted) {
        runs_.push_back(std::move(sorted));
        return runs_.size() - 1;
    }

    std::size_t spill_sorted_run_owned_admitted(
            std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted,
            std::uint64_t,
            bioseq::parallelism::MemoryPermit permit) {
        permit.release();
        runs_.push_back(std::move(sorted));
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

    std::size_t spill_sorted_segment_run(
            std::uint32_t position_segment,
            std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted) {
        auto& runs = segment_runs_[position_segment];
        runs.emplace_back(sorted.begin(), sorted.end());
        segment_spill_bytes_ += static_cast<std::uint64_t>(sorted.size())
            * sizeof(std::pair<std::uint64_t, std::uint64_t>);
        return runs.size() - 1u;
    }

    bool reroute_segment_runs(
            std::uint64_t old_span,
            std::uint64_t new_span,
            std::uint64_t target_length) {
        if (old_span == 0 || new_span == 0) {
            return false;
        }
        ++segment_reroute_calls_;
        decltype(segment_runs_) rerouted;
        for (const auto& [old_segment, runs] : segment_runs_) {
            for (const auto& run : runs) {
                ++segment_reroute_run_visits_;
                segment_reroute_bytes_visited_ +=
                    static_cast<std::uint64_t>(run.size())
                    * sizeof(std::pair<std::uint64_t, std::uint64_t>);
                std::map<
                    std::uint32_t,
                    std::vector<std::pair<std::uint64_t, std::uint64_t>>>
                    routed_run;
                for (const auto& pair : run) {
                    const std::uint64_t position = pair.second >> 1u;
                    const std::uint64_t new_segment = position / new_span;
                    if (position >= target_length
                        || position / old_span != old_segment
                        || new_segment
                            > (std::numeric_limits<std::uint32_t>::max)()) {
                        return false;
                    }
                    routed_run[static_cast<std::uint32_t>(new_segment)]
                        .push_back(pair);
                }
                for (auto& [new_segment, routed] : routed_run) {
                    if (!routed.empty()) {
                        rerouted[new_segment].push_back(std::move(routed));
                    }
                }
            }
        }
        segment_runs_ = std::move(rerouted);
        return true;
    }

    /// Segment-major fake of the PostgreSQL bounded merge. It materializes at
    /// most one immutable persisted segment and visits every qualified run
    /// exactly once per call, never every run once per segment.
    template <class F>
    void merge_segment_runs(F&& on_segment_kmer) {
        ++segment_merge_calls_;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> segment_pairs;
        std::vector<std::uint64_t> positions;
        for (const auto& [segment, runs] : segment_runs_) {
            segment_pairs.clear();
            std::size_t pair_count = 0;
            for (const auto& run : runs) {
                pair_count += run.size();
                ++segment_merge_run_visits_;
                segment_merge_bytes_visited_ +=
                    static_cast<std::uint64_t>(run.size())
                    * sizeof(std::pair<std::uint64_t, std::uint64_t>);
            }
            segment_pairs.reserve(pair_count);
            for (const auto& run : runs) {
                segment_pairs.insert(
                    segment_pairs.end(), run.begin(), run.end());
            }
            std::ranges::sort(segment_pairs);
            std::size_t index = 0;
            while (index < segment_pairs.size()) {
                const std::uint64_t hash = segment_pairs[index].first;
                positions.clear();
                while (index < segment_pairs.size()
                       && segment_pairs[index].first == hash) {
                    const std::uint64_t encoded = segment_pairs[index].second;
                    if (positions.empty() || positions.back() != encoded) {
                        positions.push_back(encoded);
                    }
                    ++index;
                }
                Cursor cursor{&positions};
                on_segment_kmer(segment, hash, cursor);
            }
        }
    }

    [[nodiscard]] std::uint64_t segment_spill_run_count() const noexcept {
        std::uint64_t count = 0;
        for (const auto& [segment, runs] : segment_runs_) {
            (void)segment;
            count += runs.size();
        }
        return count;
    }
    [[nodiscard]] std::uint64_t segment_spill_bytes() const noexcept {
        return segment_spill_bytes_;
    }
    [[nodiscard]] std::uint64_t segment_merge_calls() const noexcept {
        return segment_merge_calls_;
    }
    [[nodiscard]] std::uint64_t segment_merge_run_visits() const noexcept {
        return segment_merge_run_visits_;
    }
    [[nodiscard]] std::uint64_t segment_merge_bytes_visited() const noexcept {
        return segment_merge_bytes_visited_;
    }
    [[nodiscard]] std::uint64_t segment_reroute_calls() const noexcept {
        return segment_reroute_calls_;
    }
    [[nodiscard]] std::uint64_t segment_reroute_run_visits() const noexcept {
        return segment_reroute_run_visits_;
    }
    [[nodiscard]] std::uint64_t segment_reroute_bytes_visited() const noexcept {
        return segment_reroute_bytes_visited_;
    }

    void release_resident_runs_noexcept() noexcept { runs_.clear(); }
    void release_runs_noexcept() noexcept {
        runs_.clear();
        segment_runs_.clear();
        segment_spill_bytes_ = 0;
        segment_merge_calls_ = 0;
        segment_merge_run_visits_ = 0;
        segment_merge_bytes_visited_ = 0;
        segment_reroute_calls_ = 0;
        segment_reroute_run_visits_ = 0;
        segment_reroute_bytes_visited_ = 0;
    }
    void release_runs() { release_runs_noexcept(); }
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
static_assert(requires (
        FakeFragmentStorage<>& storage,
        std::span<const bioseq::indexing::IndexSubKeyRange> ranges,
        std::span<const bioseq::indexing::IndexSubKeyRequest> requests) {
    { storage.read_chunks_in_range(
          ranges, requests,
          bioseq::indexing::IndexChunkCachePolicy::bypass_one_shot,
          bioseq::indexing::index_qualified_chunk_range_probe{}) }
        -> std::same_as<bioseq::indexing::IndexChunkReadStats>;
});
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  FakeSequenceStorage, bioseq::indexing::SequenceIndexType::INLINE_PER_SEQUENCE>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  FakeTableStorage, bioseq::indexing::SequenceIndexType::CHUNKED_PER_TABLE>);

}  // namespace bioseq::indexing::test
