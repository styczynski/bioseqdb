/**
 * @file indexing_storage_fakes.cppm
 * @brief In-memory storage for tests that run index code without PostgreSQL.
 */

module;

#include <algorithm>
#include <atomic>
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

export module bioseq.tests.support.indexing_storage_fakes;

import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;
import bioseq.parallelism.memory_admission;

export namespace bioseq::indexing::test {

/** Reports invalid requests or corrupt chunks in a qualified read test. */
class fake_qualified_chunk_read_error final : public std::runtime_error {
public:
    /** Keeps the read failure message in the exception. */
    explicit fake_qualified_chunk_read_error(const std::string& message)
        : std::runtime_error(message) {}
};

// Fragment chunks, addressed by generation, position segment, subkey and chunk number.
// Methods without a generation argument use generation 0 and segment 0.
// Tests can lower p_max_chunk_size to exercise writes across chunk boundaries.
/** Stores fragment chunks and sorted runs in memory for index tests. */
template <std::size_t p_max_chunk_size = bioseq::indexing::k_max_index_chunk_size_bytes>
struct fake_fragment_storage {
    /** Identifies a chunk by generation, position segment, subkey and chunk number. */
    using qualified_chunk_key_t =
        std::tuple<std::uint64_t, std::int32_t, index_sub_key_t, std::uint32_t>;

    /** Stores payload bytes with the metadata checked by qualified reads. */
    struct qualified_chunk_row {
        std::uint32_t chunk_bytes = 0;
        std::uint64_t checksum = 0;
        std::vector<char> blob;
    };

    /** Counts cache lookups and insertions simulated by qualified reads. */
    struct cache_touch_probe {
        std::uint64_t lookups = 0;
        std::uint64_t populations = 0;
    };

    // Stored rows. Calls without generation arguments use generation 0 and segment 0.
    std::map<qualified_chunk_key_t, qualified_chunk_row> chunks_;
    // Directory entries that allow a range read to skip rows between requested subkeys.
    std::set<qualified_chunk_key_t> directory_declared_chunk_keys_;
    cache_touch_probe cache_touch_probe_;
    // Chunk sizes requested by resize or resize_chunks for the next write.
    std::map<std::int64_t, std::vector<std::size_t>> planned_;
    // Copies reads into int64_t-aligned storage. The next read replaces this buffer.
    mutable std::vector<std::int64_t> read_buf_;

    /** Forms a map key from a qualified chunk address. */
    [[nodiscard]] static constexpr qualified_chunk_key_t qualified_key(
            const index_chunk_address& address) noexcept {
        return {address.generation_id, address.position_segment,
                address.sub_key, address.chunk_no};
    }

    /** Builds an address with generation 0 and position segment 0. */
    [[nodiscard]] static constexpr qualified_chunk_key_t default_plane_key(
            index_sub_key_t sub_key,
            std::size_t chunk_no) noexcept {
        return {0, 0, sub_key, static_cast<std::uint32_t>(chunk_no)};
    }

    /** Checksums the chunk address, declared size and payload together. */
    [[nodiscard]] static std::uint64_t payload_row_checksum(
            const index_chunk_address& address,
            std::uint32_t chunk_bytes,
            std::span<const char> payload) noexcept {
        return bioseq::indexing::crc64_index_chunk_row(
            address.generation_id, address.position_segment, address.sub_key,
            address.chunk_no, chunk_bytes, std::as_bytes(payload));
    }

    /** Stores a payload with its byte count and row checksum. */
    [[nodiscard]] static qualified_chunk_row make_chunk_row(
            const index_chunk_address& address,
            std::vector<char> blob) {
        if (blob.size() > static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            throw fake_qualified_chunk_read_error("fake chunk exceeds uint32 metadata");
        }
        qualified_chunk_row row;
        row.chunk_bytes = static_cast<std::uint32_t>(blob.size());
        row.checksum = payload_row_checksum(
            address, row.chunk_bytes, std::span<const char>{blob});
        row.blob = std::move(blob);
        return row;
    }

    /** Throws fake_qualified_chunk_read_error with the failure message. */
    [[noreturn]] static void qualified_read_error(std::string_view message) {
        throw fake_qualified_chunk_read_error(std::string{message});
    }

    // Error and validation methods used by the index readers.
    /** Reports corruption through the indexing error handler. */
    [[noreturn]] void index_error(std::string_view m) const { bioseq::indexing::index_corruption_panic(m); }

    /** Checks the fragment header and total size, then returns the header. */
    [[nodiscard]] minimizer_hashmap_fragment_index::encoded_header
    validate_minimizer_blob_or_error(
            const char* p, std::size_t bytes,
            std::optional<std::uint32_t> expected_bucket_count = std::nullopt) const {
        using header_type = minimizer_hashmap_fragment_index::encoded_header;
        using bucket_type = minimizer_hashmap_fragment_index::encoded_bucket;
        if (bytes < sizeof(header_type)) index_error("minimizer blob truncated header");
        header_type header{};
        std::memcpy(&header, p, sizeof(header_type));
        if (header.magic != minimizer_hashmap_fragment_index::k_encoded_magic
            || header.layout_revision != minimizer_hashmap_fragment_index::k_encoded_layout_revision)
            index_error("minimizer blob magic/revision");
        const std::size_t expect = sizeof(header_type)
            + sizeof(bucket_type) * header.bucket_count
            + sizeof(std::uint64_t) * header.total_positions;
        if (bytes != expect) index_error("minimizer blob size mismatch");
        if (expected_bucket_count
            && header.bucket_count != *expected_bucket_count)
            index_error("minimizer blob bucket_count mismatch");
        return header;
    }
    /** Checks the directory header size, magic number and format revision. */
    void validate_minimizer_directory_or_error(const char* p, std::size_t bytes) const {
        if (bytes < 40) index_error("directory truncated");
        std::uint32_t magic{}, revision{};
        std::memcpy(&magic, p, 4); std::memcpy(&revision, p + 4, 4);
        if (magic != 0xB1000002u || revision != 2u) {
            index_error("directory magic/revision");
        }
    }
    /** Rejects a chunk smaller than the reader requires. */
    void validate_minimizer_chunk_metadata_or_error(index_sub_key_t sk, std::size_t chunk_no,
            std::size_t min_bytes, std::size_t /*alignment*/) const {
        if (chunk_byte_size(sk, chunk_no) < min_bytes) index_error("chunk metadata below minimum size");
    }

    // Storage methods for fragment chunks.
    /** Counts stored subkeys in generation 0, excluding directory and temporary-run keys. */
    [[nodiscard]] std::size_t number_of_sub_keys() const {

        std::size_t count = 0;
        std::int64_t last = 0; bool have_last = false;
        for (const auto& kv : chunks_) {
            if (std::get<0>(kv.first) != 0 || std::get<1>(kv.first) != 0) continue;
            const std::int64_t sk = std::get<2>(kv.first);
            if (sk < 0 || sk >= bioseq::indexing::k_temporary_run_sub_key_base) continue;
            if (!have_last || sk != last) { ++count; last = sk; have_last = true; }
        }
        return count;
    }

    /** Counts chunks from number 0 up to the first missing chunk. */
    [[nodiscard]] std::size_t number_of_chunks(index_sub_key_t sub_key) const {
        std::size_t count = 0;
        for (std::size_t c = 0;; ++c) {
            if (chunks_.find(default_plane_key(sub_key, c)) == chunks_.end()) break;
            ++count;
        }
        return count;
    }

    /** Returns a chunk payload size, or zero if the chunk is absent. */
    [[nodiscard]] std::size_t chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const {
        auto it = chunks_.find(default_plane_key(sub_key, chunk_no));
        return it == chunks_.end() ? std::size_t{0} : it->second.blob.size();
    }

    /** Copies a chunk into an int64_t-aligned buffer that lasts until the next read. */
    template <class p_element>
    [[nodiscard]] const p_element* read_chunk(
            index_sub_key_t sub_key,
            std::size_t chunk_no) {
        auto it = chunks_.find(default_plane_key(sub_key, chunk_no));
        if (it == chunks_.end()) {
            index_error("fake_fragment_storage::read_chunk: missing chunk");
        }
        const std::size_t bytes = it->second.blob.size();
        const std::size_t units = (bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        read_buf_.assign(units == 0 ? 1 : units, std::int64_t{0});
        if (bytes > 0) std::memcpy(read_buf_.data(), it->second.blob.data(), bytes);
        return reinterpret_cast<const p_element*>(read_buf_.data());
    }

    /** Plans a payload as chunks of at most p_max_chunk_size bytes. */
    void resize(index_sub_key_t sub_key, std::size_t bytes) {
        const std::size_t n_chunks = bytes == 0 ? std::size_t{1}
                                   : (bytes + p_max_chunk_size - 1) / p_max_chunk_size;
        std::vector<std::size_t> plan(n_chunks == 0 ? 1 : n_chunks);
        for (std::size_t c = 0; c + 1 < plan.size(); ++c) {
            plan[c] = p_max_chunk_size;
        }
        plan.back() = bytes - (plan.size() - 1) * p_max_chunk_size;
        planned_[sub_key] = std::move(plan);
    }

    /** Uses the supplied sizes as the chunk write plan. */
    void resize_chunks(index_sub_key_t sub_key, std::span<const std::size_t> per_chunk_bytes) {
        planned_[sub_key].assign(per_chunk_bytes.begin(), per_chunk_bytes.end());
    }

    /** Accepts prefetch calls without doing any work. */
    void prefetch_chunk(index_sub_key_t, std::size_t) {}

    /** Visits chunks in the half-open subkey range [lo, hi). */
    template <class p_callback>
    void read_chunks_in_range(
            index_sub_key_t lo,
            index_sub_key_t hi,
            p_callback&& callback) {
        if (hi <= lo) return;
        for (index_sub_key_t sub_key = lo; sub_key < hi; ++sub_key) {
            const std::size_t nc = number_of_chunks(sub_key);
            for (std::size_t chunk_no = 0; chunk_no < nc; ++chunk_no) {
                auto& blob = chunks_[default_plane_key(sub_key, chunk_no)].blob;
                callback(sub_key, blob.data(), blob.size());
            }
        }
    }

    /** Stores a chunk and adds its address to the test directory. */
    void store_qualified_chunk_for_test(
            const index_chunk_address& address,
            std::span<const char> payload) {
        std::vector<char> blob(payload.begin(), payload.end());
        chunks_[qualified_key(address)] = make_chunk_row(address, std::move(blob));
        directory_declared_chunk_keys_.insert(qualified_key(address));
    }

    /** Stores a chunk without adding a directory entry, for tests of unexpected rows. */
    void store_undeclared_qualified_chunk_for_test(
            const index_chunk_address& address,
            std::span<const char> payload) {
        std::vector<char> blob(payload.begin(), payload.end());
        chunks_[qualified_key(address)] = make_chunk_row(address, std::move(blob));
    }

    /** Lets a test alter a stored row to simulate corrupt metadata or payload. */
    [[nodiscard]] qualified_chunk_row& qualified_chunk_row_for_test(
            const index_chunk_address& address) {
        auto it = chunks_.find(qualified_key(address));
        if (it == chunks_.end()) {
            qualified_read_error("qualified test chunk is missing");
        }
        return it->second;
    }

    /** Removes a stored row while leaving its directory entry in place. */
    void erase_qualified_chunk_for_test(const index_chunk_address& address) {
        chunks_.erase(qualified_key(address));
    }

    /** Resets the simulated cache counters. */
    void reset_cache_touch_probe_for_test() noexcept {
        cache_touch_probe_ = {};
    }

    /** Returns the simulated cache counters. */
    [[nodiscard]] cache_touch_probe cache_touch_probe_for_test() const noexcept {
        return cache_touch_probe_;
    }

    /**
     * Reads the requested chunk ranges and checks their rows against the directory.
     *
     * Each row passes size and checksum checks before reaching the callback.
     * Missing trailing chunks and each request's total size and checksum are checked
     * afterwards, so the caller must discard collected data if the read throws.
     */
    template <index_chunk_reader_fn p_sink>
    [[nodiscard]] index_chunk_read_stats read_chunks_in_range(
            std::span<const index_sub_key_range> sorted_nonoverlapping_ranges,
            std::span<const index_sub_key_request> sorted_unique_requests,
            index_chunk_cache_policy cache_policy,
            p_sink&& sink) {
        using subkey_identity_t =
            std::tuple<std::uint64_t, std::int32_t, index_sub_key_t>;

        struct request_state {
            index_sub_key_request request;
            std::uint64_t end_chunk_no = 0;
            std::uint64_t next_chunk_no = 0;
            std::uint64_t total_bytes = 0;
            crc64_ecma182_accumulator checksum;
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
            case index_chunk_cache_policy::populate_residency:
            case index_chunk_cache_policy::bypass_one_shot:
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
            const index_sub_key_range& range = sorted_nonoverlapping_ranges[i];
            validate_qualifier(range.generation_id, range.position_segment);
            if (range.begin >= range.end) {
                qualified_read_error("qualified read range is empty or reversed");
            }
            if (i == 0) continue;
            const index_sub_key_range& previous = sorted_nonoverlapping_ranges[i - 1];
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

        std::vector<request_state> states;
        states.reserve(sorted_unique_requests.size());
        std::set<subkey_identity_t> wanted_subkeys;
        std::uint64_t wanted_chunks = 0;
        constexpr std::uint64_t max_sql_chunk_no = static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)());

        for (std::size_t i = 0; i < sorted_unique_requests.size(); ++i) {
            const index_sub_key_request& request = sorted_unique_requests[i];
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
                const index_sub_key_request& previous = sorted_unique_requests[i - 1];
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
            for (const index_sub_key_range& range : sorted_nonoverlapping_ranges) {
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
            states.push_back(request_state{
                .request = request,
                .end_chunk_no = end_chunk_no,
                .next_chunk_no = request.first_chunk_no,
            });
        }

        for (const index_sub_key_range& range : sorted_nonoverlapping_ranges) {
            bool covers_request = false;
            for (const index_sub_key_request& request : sorted_unique_requests) {
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

        index_chunk_read_stats stats;
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
            for (const index_sub_key_range& range : sorted_nonoverlapping_ranges) {
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

        for (const index_sub_key_range& range : sorted_nonoverlapping_ranges) {
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
                request_state* matched = nullptr;
                for (request_state& state : states) {
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
                const qualified_chunk_row& row = it->second;
                if (row.chunk_bytes > max_sql_chunk_no
                    || row.chunk_bytes != row.blob.size()) {
                    qualified_read_error("qualified read row byte-count metadata disagrees");
                }
                const std::span<const char> payload{row.blob};
                const index_chunk_address address{
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

                if (cache_policy == index_chunk_cache_policy::populate_residency) {
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

        for (const request_state& state : states) {
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
            qualified_read_error("qualified read validation-counter equation failed");
        }
        return stats;
    }

    /** Visits stored fragment subkeys in generation 0, excluding directory and temporary-run keys. */
    template <class p_callback>
    void for_each_sub_key(p_callback&& callback) const {
        std::int64_t last = 0; bool have_last = false;
        for (const auto& kv : chunks_) {
            if (std::get<0>(kv.first) != 0 || std::get<1>(kv.first) != 0) continue;
            const std::int64_t sk = std::get<2>(kv.first);
            if (sk < 0 || sk >= bioseq::indexing::k_temporary_run_sub_key_base) continue;
            if (!have_last || sk != last) {
                callback(sk);
                last = sk;
                have_last = true;
            }
        }
    }

    /** Fills planned chunks through the writer and stores their checksums. */
    template <class p_element, class p_writer>
    void write_to_each(index_sub_key_t sub_key, p_writer&& writer) {
        auto pit = planned_.find(sub_key);
        if (pit == planned_.end()) {
            index_error(
                "fake_fragment_storage::write_to_each: sub_key not planned");
        }
        const std::vector<std::size_t>& plan = pit->second;
        std::size_t chunk_no = 0;
        while (chunk_no < plan.size()) {
            const std::size_t payload_bytes = plan[chunk_no];
            if (payload_bytes % sizeof(p_element) != 0) {
                index_error(
                    "fake_fragment_storage::write_to_each: chunk byte plan not aligned");
            }
            std::vector<char> buf(payload_bytes, char{0});
            const std::size_t next = writer((std::size_t)chunk_no,
                                            reinterpret_cast<p_element*>(buf.data()),
                                            payload_bytes / sizeof(p_element));
            const index_chunk_address address{
                .generation_id = 0,
                .position_segment = 0,
                .sub_key = sub_key,
                .chunk_no = static_cast<std::uint32_t>(chunk_no),
            };
            chunks_[default_plane_key(sub_key, chunk_no)] =
                make_chunk_row(address, std::move(buf));
            chunk_no = (next == chunk_no) ? chunk_no + 1 : next;
        }
    }

    /** Clears fragment rows, directory entries, cache counters and write plans. */
    void clear_entity() {
        chunks_.clear();
        directory_declared_chunk_keys_.clear();
        cache_touch_probe_ = {};
        planned_.clear();
    }

    /** Removes rows, directory entries and write plans for subkeys in [lo, hi]. */
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

    /** Clears fragment storage. The label argument is unused. */
    void clear(std::string_view) { clear_entity(); }

    // Sorted runs used by the external-sort tests.
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
    inline static std::atomic<std::uint64_t>
        next_segment_spill_generation_token_{1u};
    /** Records which storage instance owns a spill generation. */
    struct segment_spill_binding_state {
        direct_minimizer_spill_storage_binding binding{};
        const fake_fragment_storage* owner = nullptr;

        segment_spill_binding_state() = default;
        segment_spill_binding_state(
            const segment_spill_binding_state&) = default;
        segment_spill_binding_state& operator=(
            const segment_spill_binding_state&) = default;

        /**
         * Clears the source and destination bindings when storage is moved.
         * A new generation starts with empty run storage.
         */
        segment_spill_binding_state(
                segment_spill_binding_state&& other) noexcept {
            other.binding = {};
            other.owner = nullptr;
        }
        segment_spill_binding_state& operator=(
                segment_spill_binding_state&& other) noexcept {
            binding = {};
            owner = nullptr;
            other.binding = {};
            other.owner = nullptr;
            return *this;
        }
    } segment_spill_binding_state_{};

    /** Sets the next generation token to exercise exhaustion behavior. */
    static void set_next_segment_spill_generation_token_for_test(
            std::uint64_t token) noexcept {
        next_segment_spill_generation_token_.store(
            token, std::memory_order_relaxed);
    }

    /** Clears segmented runs and counters, then starts a new generation if tokens remain. */
    [[nodiscard]] direct_minimizer_spill_storage_binding
    begin_segment_spill_generation() noexcept {
        for (auto& [segment, runs] : segment_runs_) {
            (void)segment;
            runs.clear();
        }
        segment_runs_.clear();
        segment_spill_bytes_ = 0;
        segment_merge_calls_ = 0;
        segment_merge_run_visits_ = 0;
        segment_merge_bytes_visited_ = 0;
        segment_reroute_calls_ = 0;
        segment_reroute_run_visits_ = 0;
        segment_reroute_bytes_visited_ = 0;
        std::uint64_t token =
            next_segment_spill_generation_token_.load(
                std::memory_order_relaxed);
        for (;;) {
            if (token == 0) {
                segment_spill_binding_state_ = {};
                return {};
            }
            const std::uint64_t next = token
                    == (std::numeric_limits<std::uint64_t>::max)()
                ? 0u : token + 1u;
            if (next_segment_spill_generation_token_.compare_exchange_weak(
                    token,
                    next,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;
            }
        }
        segment_spill_binding_state_.binding = direct_minimizer_spill_storage_binding{
            .generation_token = token,
            .mutation_revision = 0,
        };
        segment_spill_binding_state_.owner = this;
        return segment_spill_binding_state_.binding;
    }

    /** Returns the binding owned by this storage instance, if any. */
    [[nodiscard]] direct_minimizer_spill_storage_binding
    segment_spill_storage_binding() const noexcept {
        return segment_spill_binding_state_.owner == this
            ? segment_spill_binding_state_.binding
            : direct_minimizer_spill_storage_binding{};
    }

    /** Sets the mutation revision for a bound test generation. */
    void set_segment_spill_mutation_revision_for_test(
            std::uint64_t mutation_revision) noexcept {
        if (segment_spill_binding_state_.owner == this) {
            segment_spill_binding_state_.binding.mutation_revision =
                mutation_revision;
        }
    }

    /** Records a change to the runs, invalidating the binding if its revision overflows. */
    void record_segment_spill_mutation_() noexcept {
        if (segment_spill_binding_state_.owner != this
            || !segment_spill_binding_state_.binding.valid()) {
            return;
        }
        if (segment_spill_binding_state_.binding.mutation_revision
                == (std::numeric_limits<std::uint64_t>::max)()) {
            segment_spill_binding_state_ = {};
            return;
        }
        ++segment_spill_binding_state_.binding.mutation_revision;
    }

    /** Copies a sorted run into fake spill storage. */
    std::size_t spill_sorted_run(std::span<const std::pair<std::uint64_t, std::uint64_t>> s) {
        runs_.push_back(std::vector<std::pair<std::uint64_t, std::uint64_t>>(s.begin(), s.end()));
        return runs_.size() - 1;
    }

    /** Moves an owned sorted run into fake spill storage. */
    std::size_t spill_sorted_run_owned(
            std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted) {
        runs_.push_back(std::move(sorted));
        return runs_.size() - 1;
    }

    /** Stores a run in the fake after returning its memory permit. */
    std::size_t spill_sorted_run_owned_admitted(
            std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted,
            std::uint64_t,
            bioseq::parallelism::memory_permit permit) {
        permit.release();
        runs_.push_back(std::move(sorted));
        return runs_.size() - 1;
    }

    /** Gives a merge callback access to one hash's sorted positions. */
    struct cursor {
        const std::vector<std::uint64_t>* v;
        /** Returns the number of positions exposed by this cursor. */
        [[nodiscard]] std::uint64_t total_count() const { return v->size(); }
        /** Visits the cursor positions as one batch when nonempty. */
        template <class p_callback>
        void for_each_batch(p_callback&& callback) const {
            if (!v->empty()) {
                callback(std::span<const std::uint64_t>(v->data(), v->size()));
            }
        }
    };

    /** Calls the reader once per hash with its sorted, distinct encoded positions. */
    template <class p_callback>
    void merge_runs(p_callback&& on_kmer) {
        // The fake sorts all runs together in memory to produce the merge result.
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
                    positions.push_back(all[j].second);
                ++j;
            }
            cursor current{&positions};
            on_kmer(kmer, current);
            i = j;
        }
    }

    /** Copies a sorted run into one position segment and records the storage change. */
    std::size_t spill_sorted_segment_run(
            std::uint32_t position_segment,
            std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted) {
        if (segment_spill_binding_state_.owner != this
            || !segment_spill_binding_state_.binding.valid()) {
            (void)begin_segment_spill_generation();
        }
        auto& runs = segment_runs_[position_segment];
        runs.emplace_back(sorted.begin(), sorted.end());
        segment_spill_bytes_ += static_cast<std::uint64_t>(sorted.size())
            * sizeof(std::pair<std::uint64_t, std::uint64_t>);
        record_segment_spill_mutation_();
        return runs.size() - 1u;
    }

    /** Moves runs to new position segments if every position fits the supplied bounds. */
    bool reroute_segment_runs(
            std::uint64_t source_span,
            std::uint64_t target_span,
            std::uint64_t target_length) {
        if (segment_spill_binding_state_.owner != this
            || !segment_spill_binding_state_.binding.valid()
            || source_span == 0 || target_span == 0) {
            return false;
        }
        ++segment_reroute_calls_;
        decltype(segment_runs_) rerouted;
        for (const auto& [source_segment, runs] : segment_runs_) {
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
                    const std::uint64_t target_segment = position / target_span;
                    if (position >= target_length
                        || position / source_span != source_segment
                        || target_segment
                            > (std::numeric_limits<std::uint32_t>::max)()) {
                        return false;
                    }
                    routed_run[static_cast<std::uint32_t>(target_segment)]
                        .push_back(pair);
                }
                for (auto& [target_segment, routed] : routed_run) {
                    if (!routed.empty()) {
                        rerouted[target_segment].push_back(std::move(routed));
                    }
                }
            }
        }
        segment_runs_ = std::move(rerouted);
        record_segment_spill_mutation_();
        return true;
    }

    /**
     * Combines each segment's runs without changing the stored events.
     * Leaves the generation binding unchanged because its events are unchanged.
     */
    void compact_segment_spill_representation_preserving() {
        for (auto& [segment, runs] : segment_runs_) {
            (void)segment;
            if (runs.size() <= 1u) {
                continue;
            }
            std::vector<std::pair<std::uint64_t, std::uint64_t>> compacted;
            std::size_t pair_count = 0;
            for (const auto& run : runs) {
                pair_count += run.size();
            }
            compacted.reserve(pair_count);
            for (const auto& run : runs) {
                compacted.insert(compacted.end(), run.begin(), run.end());
            }
            std::ranges::sort(compacted);
            runs.clear();
            runs.push_back(std::move(compacted));
        }
    }

    /**
     * Calls the reader for each segment and hash with sorted, distinct positions.
     * Records the runs and bytes read during the merge.
     */
    template <class p_callback>
    void merge_segment_runs(p_callback&& on_segment_kmer) {
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
                cursor current{&positions};
                on_segment_kmer(segment, hash, current);
            }
        }
    }

    /** Returns the number of physical runs across all position segments. */
    [[nodiscard]] std::uint64_t segment_spill_run_count() const noexcept {
        std::uint64_t count = 0;
        for (const auto& [segment, runs] : segment_runs_) {
            (void)segment;
            count += runs.size();
        }
        return count;
    }
    /** Returns cumulative bytes appended to segmented spill storage. */
    [[nodiscard]] std::uint64_t segment_spill_bytes() const noexcept {
        return segment_spill_bytes_;
    }
    /** Returns the number of segmented merge calls. */
    [[nodiscard]] std::uint64_t segment_merge_calls() const noexcept {
        return segment_merge_calls_;
    }
    /** Returns the number of runs visited by segmented merges. */
    [[nodiscard]] std::uint64_t segment_merge_run_visits() const noexcept {
        return segment_merge_run_visits_;
    }
    /** Returns the bytes visited by segmented merges. */
    [[nodiscard]] std::uint64_t segment_merge_bytes_visited() const noexcept {
        return segment_merge_bytes_visited_;
    }
    /** Returns the number of reroute attempts made with a valid binding and nonzero spans. */
    [[nodiscard]] std::uint64_t segment_reroute_calls() const noexcept {
        return segment_reroute_calls_;
    }
    /** Returns the number of runs visited while rerouting segments. */
    [[nodiscard]] std::uint64_t segment_reroute_run_visits() const noexcept {
        return segment_reroute_run_visits_;
    }
    /** Returns the bytes visited while rerouting segments. */
    [[nodiscard]] std::uint64_t segment_reroute_bytes_visited() const noexcept {
        return segment_reroute_bytes_visited_;
    }

    /** Clears run payloads and records the change while keeping the current counters. */
    void release_resident_runs_noexcept() noexcept {
        runs_.clear();
        for (auto& [segment, runs] : segment_runs_) {
            (void)segment;
            runs.clear();
        }
        segment_runs_.clear();
        record_segment_spill_mutation_();
    }
    /** Clears all runs and counters, then records the storage change. */
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
        record_segment_spill_mutation_();
    }
    /** Clears all runs and resets the spill counters. */
    void release_runs() { release_runs_noexcept(); }
    /** Returns the configured in-memory run buffer size. */
    [[nodiscard]] std::size_t run_buffer_bytes() const { return run_buffer_bytes_; }
};

// Inline sequence storage.
/** Stores one inline sequence payload for index tests. */
struct fake_sequence_storage {
    std::vector<char> chunk_;
    std::size_t planned_bytes_ = 0;
    bool has_plan_ = false;
    mutable std::vector<std::int64_t> read_buf_;

    /** Reports corruption through the indexing error handler. */
    [[noreturn]] void index_error(std::string_view m) const { bioseq::indexing::index_corruption_panic(m); }

    /** Returns 1 for a nonempty payload and 0 for an empty payload. */
    [[nodiscard]] std::size_t number_of_chunks() const { return chunk_.empty() ? std::size_t{0} : std::size_t{1}; }
    /** Returns the inline chunk payload size. */
    [[nodiscard]] std::size_t chunk_byte_size() const { return chunk_.size(); }

    /** Copies the sequence into an int64_t-aligned buffer that lasts until the next read. */
    template <class p_element>
    [[nodiscard]] const p_element* read_chunk() {
        const std::size_t bytes = chunk_.size();
        const std::size_t units = (bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        read_buf_.assign(units == 0 ? 1 : units, std::int64_t{0});
        if (bytes > 0) std::memcpy(read_buf_.data(), chunk_.data(), bytes);
        return reinterpret_cast<const p_element*>(read_buf_.data());
    }

    /** Plans one inline chunk, rejecting payloads above the chunk size limit. */
    void resize(std::size_t bytes) {
        if (bytes > bioseq::indexing::k_max_index_chunk_size_bytes)
            index_error(
                "fake_sequence_storage::resize: inline chunk exceeds "
                "k_max_index_chunk_size_bytes");
        planned_bytes_ = bytes;
        has_plan_ = true;
    }

    /** Fills the planned inline chunk through one writer callback. */
    template <class p_element, class p_writer>
    void write_to_each(p_writer&& writer) {
        if (!has_plan_) {
            index_error(
                "fake_sequence_storage::write_to_each: no plan "
                "(call resize first)");
        }
        if (planned_bytes_ % sizeof(p_element) != 0) {
            index_error(
                "fake_sequence_storage::write_to_each: planned bytes not aligned");
        }
        std::vector<char> buf(planned_bytes_, char{0});
        const std::size_t next = writer(
            (std::size_t)0,
            reinterpret_cast<p_element*>(buf.data()),
            planned_bytes_ / sizeof(p_element));
        if (next != 0) {
            index_error(
                "fake_sequence_storage::write_to_each: inline storage has "
                "exactly one chunk");
        }
        chunk_ = std::move(buf);
    }

    /** Accepts prefetch calls without doing any work. */
    void prefetch_chunk() {}
    /** Clears the inline chunk and its pending write plan. */
    void clear_entity() { chunk_.clear(); planned_bytes_ = 0; has_plan_ = false; }
    /** Clears the inline payload and plan. The label argument is unused. */
    void clear(std::string_view) { clear_entity(); }
};

// Table storage, with numbered chunks and sequence row traversal.
/** Stores table-index chunks and test sequence row numbers in memory. */
struct fake_table_storage {
    std::map<std::size_t, std::vector<char>> chunks_;
    std::vector<std::size_t> planned_;
    std::vector<int> seq_rownos_;          // Row numbers passed to for_each_sequence.
    mutable std::vector<std::int64_t> read_buf_;

    /** Reports corruption through the indexing error handler. */
    [[noreturn]] void index_error(std::string_view m) const { bioseq::indexing::index_corruption_panic(m); }

    /** Returns the number of stored table chunks. */
    [[nodiscard]] std::size_t number_of_chunks() const { return chunks_.size(); }
    /** Returns the payload size for a table chunk, or zero when absent. */
    [[nodiscard]] std::size_t chunk_byte_size(std::size_t chunk_no) const {
        auto it = chunks_.find(chunk_no);
        return it == chunks_.end() ? std::size_t{0} : it->second.size();
    }

    /** Copies a table chunk into an int64_t-aligned buffer that lasts until the next read. */
    template <class p_element>
    [[nodiscard]] const p_element* read_chunk(std::size_t chunk_no) {
        auto it = chunks_.find(chunk_no);
        if (it == chunks_.end()) {
            index_error("fake_table_storage::read_chunk: missing chunk");
        }
        const std::size_t bytes = it->second.size();
        const std::size_t units = (bytes + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        read_buf_.assign(units == 0 ? 1 : units, std::int64_t{0});
        if (bytes > 0) std::memcpy(read_buf_.data(), it->second.data(), bytes);
        return reinterpret_cast<const p_element*>(read_buf_.data());
    }

    /** Divides a table payload into chunks up to the index chunk size limit. */
    void resize(std::size_t bytes) {
        const std::size_t n_chunks = bytes == 0 ? std::size_t{1}
                                   : (bytes + bioseq::indexing::k_max_index_chunk_size_bytes - 1)
                                       / bioseq::indexing::k_max_index_chunk_size_bytes;
        planned_.assign(n_chunks == 0 ? 1 : n_chunks, std::size_t{0});
        for (std::size_t c = 0; c + 1 < planned_.size(); ++c) planned_[c] = bioseq::indexing::k_max_index_chunk_size_bytes;
        planned_.back() = bytes - (planned_.size() - 1) * bioseq::indexing::k_max_index_chunk_size_bytes;
    }

    /** Accepts prefetch calls without doing any work. */
    void prefetch_chunk(std::size_t) {}

    /** Fills each planned table chunk through the writer. */
    template <class p_element, class p_writer>
    void write_to_each(p_writer&& writer) {
        for (std::size_t chunk_no = 0; chunk_no < planned_.size(); ++chunk_no) {
            const std::size_t payload_bytes = planned_[chunk_no];
            if (payload_bytes % sizeof(p_element) != 0) {
                index_error(
                    "fake_table_storage::write_to_each: chunk not aligned");
            }
            std::vector<char> buf(payload_bytes, char{0});
            (void)writer(
                (std::size_t)chunk_no,
                reinterpret_cast<p_element*>(buf.data()),
                payload_bytes / sizeof(p_element));
            chunks_[chunk_no] = std::move(buf);
        }
    }

    /** Fills the selected table chunk using its planned size. */
    template <class p_element, class p_writer>
    void write_to(std::size_t chunk_no, p_writer&& writer) {
        if (chunk_no >= planned_.size()) {
            index_error(
                "fake_table_storage::write_to: chunk_no past plan "
                "(call resize first)");
        }
        const std::size_t payload_bytes = planned_[chunk_no];
        if (payload_bytes % sizeof(p_element) != 0) {
            index_error("fake_table_storage::write_to: chunk not aligned");
        }
        std::vector<char> buf(payload_bytes, char{0});
        (void)writer(
            (std::size_t)chunk_no,
            reinterpret_cast<p_element*>(buf.data()),
            payload_bytes / sizeof(p_element));
        chunks_[chunk_no] = std::move(buf);
    }

    /** Calls the loader for each configured row number, with the total row count. */
    template <class p_loader>
    void for_each_sequence(p_loader&& loader) {
        // These loaders only use the row number and count. An int stands in for the sequence.
        for (std::size_t i = 0; i < seq_rownos_.size(); ++i) {
            int dummy = 0;
            loader(dummy, (std::size_t)seq_rownos_[i], (std::size_t)seq_rownos_.size());
        }
    }

    /** Clears all table chunks and pending write plans. */
    void clear(std::string_view) { chunks_.clear(); planned_.clear(); }
};

// Keep the fake storage methods compatible with the index storage concepts.
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  fake_fragment_storage<>,
                  bioseq::indexing::sequence_index_layout::fragment_chunks>);
static_assert(
    bioseq::indexing::is_external_sort_spill_storage<fake_fragment_storage<>>);
static_assert(requires (
        fake_fragment_storage<>& storage,
        std::span<const bioseq::indexing::index_sub_key_range> ranges,
        std::span<const bioseq::indexing::index_sub_key_request> requests) {
    { storage.read_chunks_in_range(
          ranges, requests,
          bioseq::indexing::index_chunk_cache_policy::bypass_one_shot,
          bioseq::indexing::index_qualified_chunk_range_probe{}) }
        -> std::same_as<bioseq::indexing::index_chunk_read_stats>;
});
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  fake_sequence_storage,
                  bioseq::indexing::sequence_index_layout::sequence_inline>);
static_assert(bioseq::indexing::is_index_chunked_storage_for<
                  fake_table_storage,
                  bioseq::indexing::sequence_index_layout::table_chunks>);

}  // namespace bioseq::indexing::test
