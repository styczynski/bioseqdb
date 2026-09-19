/**
 * @file e2e_phase_receipt_main.cpp
 * @brief Standalone bundled-minimap2 driver with an out-of-band E2E phase receipt.
 *
 * This translation unit is intentionally outside every shared benchmark target.
 * Its C-linkage hooks are referenced only when the vendored sources are compiled
 * with BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT. Mapping stdout remains PAF-only; the
 * single receipt is written to a caller-owned descriptor.
 */

#if !defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
#error "e2e_phase_receipt_main.cpp requires BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "minimap2/instr_shim.h"
#include "minimap2/minimap.h"
#include "minimap2/mmpriv.h"

namespace {

constexpr std::uint32_t k_phase_cigar = BIOSEQ_MINIMAP2_E2E_PHASE_CIGAR;
constexpr std::uint32_t k_phase_mapq = BIOSEQ_MINIMAP2_E2E_PHASE_MAPQ;
constexpr std::size_t k_interval_capacity = 262'144;
constexpr std::string_view k_schema = "bioseq.minimap2.e2e-phase-receipt";
constexpr std::uint64_t k_schema_version = 2;
constexpr std::string_view k_checksum_algorithm = "fnv-1a-64";
constexpr std::string_view k_checksum_canonicalization =
    "bioseq.minimap2.e2e-phase-receipt.checksum.v1";

enum class RunMode {
    index_setup,
    no_index,
    prebuilt_index,
};

[[nodiscard]] std::string_view mode_name(RunMode mode) noexcept {
    switch (mode) {
    case RunMode::index_setup:
        return "index_setup";
    case RunMode::no_index:
        return "no_index";
    case RunMode::prebuilt_index:
        return "prebuilt_index";
    }
    return "invalid";
}

struct RawInterval {
    std::uint64_t begin_ns{};
    std::uint64_t end_ns{};
    std::uint32_t phase{};
    std::uint32_t thread_ordinal{};
};

struct RawSlot {
    RawInterval interval{};
    std::atomic<std::uint8_t> committed{};
};

struct RuntimeState {
    std::array<RawSlot, k_interval_capacity> slots{};
    std::atomic<std::uint64_t> claimed_slots{};
    std::atomic<std::uint64_t> begin_calls{};
    std::atomic<std::uint64_t> end_calls{};
    std::atomic<std::uint64_t> cigar_begin_calls{};
    std::atomic<std::uint64_t> mapq_begin_calls{};
    std::atomic<std::uint32_t> next_thread_ordinal{};
    std::atomic<bool> active{};
    std::atomic<bool> started{};
    std::atomic<bool> clock_valid{true};
    std::atomic<bool> interval_valid{true};
    std::atomic<bool> buffer_overflow{};
    std::atomic<bool> late_event{};
    std::atomic<bool> split_io_error{};
};

RuntimeState g_runtime;

struct OperationResult {
    bool complete{};
    std::uint64_t observed_index_parts{};
    std::string parent_cleanup_root;
    std::string split_temp_directory;
    std::string split_prefix;
    bool cleanup_complete{true};
};

struct ReceiptCounts {
    std::uint64_t begin_calls{};
    std::uint64_t end_calls{};
    std::uint64_t cigar_begin_calls{};
    std::uint64_t mapq_begin_calls{};
    std::uint64_t claimed_slots{};
    std::uint64_t recorded_intervals{};
    std::uint64_t dropped_intervals{};
    std::uint64_t cigar_intervals{};
    std::uint64_t mapq_intervals{};
    std::uint64_t threads_observed{};
    std::uint64_t expected_queries{};
    std::uint64_t observed_queries{};
    std::uint64_t expected_index_parts{};
    std::uint64_t observed_index_parts{};
};

struct Fnv1a64 {
    static constexpr std::uint64_t offset_basis = 14'695'981'039'346'656'037ULL;
    static constexpr std::uint64_t prime = 1'099'511'628'211ULL;

    std::uint64_t value{offset_basis};

    void append_byte(std::uint8_t byte) noexcept {
        value ^= byte;
        value *= prime;
    }

    void append_u32_le(std::uint32_t number) noexcept {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<std::uint8_t>(number >> shift));
        }
    }

    void append_u64_le(std::uint64_t number) noexcept {
        for (unsigned int shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<std::uint8_t>(number >> shift));
        }
    }

    void append_string(std::string_view value_to_append) noexcept {
        append_u64_le(static_cast<std::uint64_t>(value_to_append.size()));
        for (const unsigned char byte : value_to_append) {
            append_byte(byte);
        }
    }
};

[[nodiscard]] bool monotonic_now_ns(std::uint64_t& result) noexcept {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1'000'000'000L) {
        return false;
    }

    constexpr auto billion = std::uint64_t{1'000'000'000};
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
    if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) / billion) {
        return false;
    }

    result = seconds * billion + nanoseconds;
    return result != 0;
}

[[nodiscard]] std::uint32_t current_thread_ordinal() noexcept {
    thread_local const std::uint32_t ordinal =
        g_runtime.next_thread_ordinal.fetch_add(1, std::memory_order_relaxed);
    return ordinal;
}

[[nodiscard]] bool runtime_start() noexcept {
    bool expected = false;
    if (!g_runtime.started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    g_runtime.active.store(true, std::memory_order_release);
    return true;
}

[[nodiscard]] const char* phase_name(std::uint32_t phase) noexcept {
    return phase == k_phase_cigar ? "cigar" : phase == k_phase_mapq ? "mapq" : "invalid";
}

struct UnionStats {
    std::uint64_t duration_ns{};
    std::uint64_t merged_interval_count{};
    bool arithmetic_valid{true};
};

[[nodiscard]] UnionStats interval_union(const std::vector<RawInterval>& raw,
                                        std::uint32_t selected_phase) noexcept {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    spans.reserve(raw.size());
    for (const auto& interval : raw) {
        if (selected_phase == 0 || interval.phase == selected_phase) {
            spans.emplace_back(interval.begin_ns, interval.end_ns);
        }
    }
    std::sort(spans.begin(), spans.end());

    UnionStats result{};
    if (spans.empty()) {
        return result;
    }

    auto current_begin = spans.front().first;
    auto current_end = spans.front().second;
    const auto commit_span = [&result](std::uint64_t begin, std::uint64_t end) noexcept {
        if (end < begin) {
            result.arithmetic_valid = false;
            return;
        }
        const auto duration = end - begin;
        if (duration > std::numeric_limits<std::uint64_t>::max() - result.duration_ns) {
            result.arithmetic_valid = false;
            return;
        }
        result.duration_ns += duration;
        ++result.merged_interval_count;
    };

    for (std::size_t index = 1; index < spans.size(); ++index) {
        const auto [begin, end] = spans[index];
        if (begin <= current_end) {
            current_end = std::max(current_end, end);
            continue;
        }
        commit_span(current_begin, current_end);
        current_begin = begin;
        current_end = end;
    }
    commit_span(current_begin, current_end);
    return result;
}

void append_u64(std::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto conversion = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (conversion.ec == std::errc{}) {
        output.append(buffer.data(), conversion.ptr);
    } else {
        output += '0';
    }
}

void append_bool(std::string& output, bool value) {
    output += value ? "true" : "false";
}

void append_json_string(std::string& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output += '"';
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20) {
                output += "\\u00";
                output += hex[byte >> 4];
                output += hex[byte & 0x0f];
            } else {
                output += static_cast<char>(byte);
            }
        }
    }
    output += '"';
}

void append_hex_u64(std::string& output, std::uint64_t value) {
    constexpr char hex[] = "0123456789abcdef";
    output += '"';
    for (int shift = 60; shift >= 0; shift -= 4) {
        output += hex[(value >> shift) & 0x0f];
    }
    output += '"';
}

[[nodiscard]] bool checked_add(std::uint64_t lhs,
                               std::uint64_t rhs,
                               std::uint64_t& result) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] bool checked_multiply(std::uint64_t lhs,
                                    std::uint64_t rhs,
                                    std::uint64_t& result) noexcept {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

struct ExpectedPhaseCounts {
    std::uint64_t cigar{};
    std::uint64_t mapq{};
    std::uint64_t total{};
    bool arithmetic_valid{true};
};

[[nodiscard]] ExpectedPhaseCounts expected_phase_counts(RunMode mode,
                                                        std::uint64_t query_count,
                                                        std::uint64_t index_part_count) noexcept {
    ExpectedPhaseCounts result;
    if (mode == RunMode::index_setup) {
        return result;
    }

    std::uint64_t mapq_passes_per_query = index_part_count;
    result.arithmetic_valid = index_part_count > 0;
    if (result.arithmetic_valid && index_part_count > 1) {
        result.arithmetic_valid = checked_add(index_part_count, 1, mapq_passes_per_query);
    }
    result.arithmetic_valid = result.arithmetic_valid &&
        checked_multiply(query_count, index_part_count, result.cigar) &&
        checked_multiply(query_count, mapq_passes_per_query, result.mapq) &&
        checked_add(result.cigar, result.mapq, result.total);
    return result;
}

/**
 * Canonical checksum byte stream, version 1:
 *
 *  - every string is u64-le byte length followed by its exact bytes;
 *  - every scalar is fixed-width little-endian;
 *  - fields are domain, schema, schema_version, run_token, mode, pid,
 *    ReceiptCounts in declaration order, then each sorted RawInterval as
 *    phase:u32, begin_ns:u64, end_ns:u64, thread_ordinal:u32;
 *  - the checksum is FNV-1a-64, rendered as 16 lowercase hexadecimal digits.
 */
[[nodiscard]] std::uint64_t receipt_checksum(std::string_view run_token,
                                             std::string_view mode,
                                             std::uint64_t pid,
                                             const ReceiptCounts& counts,
                                             const std::vector<RawInterval>& raw) noexcept {
    Fnv1a64 checksum;
    checksum.append_string(k_checksum_canonicalization);
    checksum.append_string(k_schema);
    checksum.append_u64_le(k_schema_version);
    checksum.append_string(run_token);
    checksum.append_string(mode);
    checksum.append_u64_le(pid);
    checksum.append_u64_le(counts.begin_calls);
    checksum.append_u64_le(counts.end_calls);
    checksum.append_u64_le(counts.cigar_begin_calls);
    checksum.append_u64_le(counts.mapq_begin_calls);
    checksum.append_u64_le(counts.claimed_slots);
    checksum.append_u64_le(counts.recorded_intervals);
    checksum.append_u64_le(counts.dropped_intervals);
    checksum.append_u64_le(counts.cigar_intervals);
    checksum.append_u64_le(counts.mapq_intervals);
    checksum.append_u64_le(counts.threads_observed);
    checksum.append_u64_le(counts.expected_queries);
    checksum.append_u64_le(counts.observed_queries);
    checksum.append_u64_le(counts.expected_index_parts);
    checksum.append_u64_le(counts.observed_index_parts);
    for (const auto& interval : raw) {
        checksum.append_u32_le(interval.phase);
        checksum.append_u64_le(interval.begin_ns);
        checksum.append_u64_le(interval.end_ns);
        checksum.append_u32_le(interval.thread_ordinal);
    }
    return checksum.value;
}

struct ReceiptResult {
    std::string json;
    bool valid{};
};

[[nodiscard]] ReceiptResult
make_receipt(std::string_view run_token,
             RunMode run_mode,
             std::uint64_t expected_queries,
             std::uint64_t expected_index_parts,
             const OperationResult& operation) {
    g_runtime.active.store(false, std::memory_order_release);

    const auto mode = mode_name(run_mode);
    const auto pid = static_cast<std::uint64_t>(::getpid());

    const auto claimed = g_runtime.claimed_slots.load(std::memory_order_acquire);
    const auto visible_slots =
        std::min<std::uint64_t>(claimed, static_cast<std::uint64_t>(k_interval_capacity));

    std::vector<RawInterval> raw;
    raw.reserve(static_cast<std::size_t>(visible_slots));
    bool buffer_valid = !g_runtime.buffer_overflow.load(std::memory_order_acquire);
    bool intervals_valid = g_runtime.interval_valid.load(std::memory_order_acquire);
    for (std::uint64_t index = 0; index < visible_slots; ++index) {
        const auto& slot = g_runtime.slots[static_cast<std::size_t>(index)];
        if (slot.committed.load(std::memory_order_acquire) == 0) {
            buffer_valid = false;
            continue;
        }
        const auto interval = slot.interval;
        if ((interval.phase != k_phase_cigar && interval.phase != k_phase_mapq) ||
            interval.end_ns < interval.begin_ns) {
            intervals_valid = false;
            continue;
        }
        raw.push_back(interval);
    }

    std::sort(raw.begin(), raw.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.begin_ns != rhs.begin_ns)
            return lhs.begin_ns < rhs.begin_ns;
        if (lhs.end_ns != rhs.end_ns)
            return lhs.end_ns < rhs.end_ns;
        if (lhs.phase != rhs.phase)
            return lhs.phase < rhs.phase;
        return lhs.thread_ordinal < rhs.thread_ordinal;
    });

    std::uint64_t cigar_count = 0;
    std::uint64_t mapq_count = 0;
    for (const auto& interval : raw) {
        if (interval.phase == k_phase_cigar)
            ++cigar_count;
        if (interval.phase == k_phase_mapq)
            ++mapq_count;
    }

    ReceiptCounts counts{
        .begin_calls = g_runtime.begin_calls.load(std::memory_order_acquire),
        .end_calls = g_runtime.end_calls.load(std::memory_order_acquire),
        .cigar_begin_calls = g_runtime.cigar_begin_calls.load(std::memory_order_acquire),
        .mapq_begin_calls = g_runtime.mapq_begin_calls.load(std::memory_order_acquire),
        .claimed_slots = claimed,
        .recorded_intervals = static_cast<std::uint64_t>(raw.size()),
        .dropped_intervals = claimed > raw.size() ? claimed - raw.size() : 0,
        .cigar_intervals = cigar_count,
        .mapq_intervals = mapq_count,
        .threads_observed = g_runtime.next_thread_ordinal.load(std::memory_order_acquire),
        .expected_queries = expected_queries,
        .expected_index_parts = expected_index_parts,
        .observed_index_parts = operation.observed_index_parts,
    };

    bool observed_query_count_valid = false;
    if (run_mode == RunMode::index_setup) {
        counts.observed_queries = 0;
        observed_query_count_valid = true;
    } else if (counts.observed_index_parts != 0 &&
               counts.cigar_begin_calls % counts.observed_index_parts == 0) {
        counts.observed_queries = counts.cigar_begin_calls / counts.observed_index_parts;
        observed_query_count_valid = true;
    }

    const auto expected =
        expected_phase_counts(run_mode, expected_queries, expected_index_parts);
    const bool capacity_contract_valid = expected.arithmetic_valid &&
        expected.total <= static_cast<std::uint64_t>(k_interval_capacity);

    const bool index_part_count_valid = expected_index_parts > 0 &&
                                        counts.observed_index_parts == expected_index_parts;
    const bool query_count_valid = run_mode == RunMode::index_setup
        ? expected_queries == 0 && counts.observed_queries == 0
        : expected_queries > 0 && observed_query_count_valid &&
              counts.observed_queries == expected_queries;
    const bool phase_count_contract_valid = run_mode == RunMode::index_setup
        ? counts.begin_calls == 0 && counts.end_calls == 0 &&
              counts.cigar_begin_calls == 0 && counts.mapq_begin_calls == 0 &&
              counts.claimed_slots == 0 && counts.recorded_intervals == 0 &&
              counts.cigar_intervals == 0 && counts.mapq_intervals == 0 &&
              counts.threads_observed == 0
        : capacity_contract_valid &&
              counts.cigar_begin_calls == expected.cigar &&
              counts.mapq_begin_calls == expected.mapq &&
              counts.cigar_intervals == expected.cigar &&
              counts.mapq_intervals == expected.mapq &&
              counts.begin_calls == expected.total &&
              counts.end_calls == expected.total &&
              counts.claimed_slots == expected.total &&
              counts.recorded_intervals == expected.total &&
              counts.dropped_intervals == 0;
    const bool workload_valid = index_part_count_valid && query_count_valid &&
                                phase_count_contract_valid;

    const auto cigar_union = interval_union(raw, k_phase_cigar);
    const auto mapq_union = interval_union(raw, k_phase_mapq);
    const auto combined_union = interval_union(raw, 0);
    const bool arithmetic_valid = expected.arithmetic_valid && cigar_union.arithmetic_valid &&
                                  mapq_union.arithmetic_valid &&
                                  combined_union.arithmetic_valid;
    const bool clock_valid = g_runtime.clock_valid.load(std::memory_order_acquire);
    const bool late_event = g_runtime.late_event.load(std::memory_order_acquire);
    const bool split_io_valid = !g_runtime.split_io_error.load(std::memory_order_acquire);
    if (counts.begin_calls != counts.end_calls) {
        intervals_valid = false;
    }
    const bool valid = operation.complete && operation.cleanup_complete && split_io_valid &&
                       clock_valid &&
                       buffer_valid && intervals_valid && arithmetic_valid && !late_event &&
                       workload_valid;
    const auto checksum = receipt_checksum(run_token, mode, pid, counts, raw);

    std::string output;
    output.reserve(1536 + raw.size() * 112);
    output += "{\"schema\":";
    append_json_string(output, k_schema);
    output += ",\"version\":";
    append_u64(output, k_schema_version);
    output += ",\"run_token\":";
    append_json_string(output, run_token);
    output += ",\"mode\":";
    append_json_string(output, mode);
    output += ",\"pid\":";
    append_u64(output, pid);
    output += ",\"clock\":\"CLOCK_MONOTONIC\",\"clock_unit\":\"ns\"";
    output += ",\"interval_semantics\":\"half_open\"";
    output += ",\"buffer_capacity\":";
    append_u64(output, k_interval_capacity);
    output += ",\"counts\":{\"begin_calls\":";
    append_u64(output, counts.begin_calls);
    output += ",\"end_calls\":";
    append_u64(output, counts.end_calls);
    output += ",\"cigar_begin_calls\":";
    append_u64(output, counts.cigar_begin_calls);
    output += ",\"mapq_begin_calls\":";
    append_u64(output, counts.mapq_begin_calls);
    output += ",\"claimed_slots\":";
    append_u64(output, counts.claimed_slots);
    output += ",\"recorded_intervals\":";
    append_u64(output, counts.recorded_intervals);
    output += ",\"dropped_intervals\":";
    append_u64(output, counts.dropped_intervals);
    output += ",\"cigar_intervals\":";
    append_u64(output, counts.cigar_intervals);
    output += ",\"mapq_intervals\":";
    append_u64(output, counts.mapq_intervals);
    output += ",\"threads_observed\":";
    append_u64(output, counts.threads_observed);
    output += ",\"expected_queries\":";
    append_u64(output, counts.expected_queries);
    output += ",\"observed_queries\":";
    append_u64(output, counts.observed_queries);
    output += ",\"expected_index_parts\":";
    append_u64(output, counts.expected_index_parts);
    output += ",\"observed_index_parts\":";
    append_u64(output, counts.observed_index_parts);
    output += "}";
    output += ",\"unions_ns\":{\"cigar\":";
    append_u64(output, cigar_union.duration_ns);
    output += ",\"mapq\":";
    append_u64(output, mapq_union.duration_ns);
    output += ",\"cigar_or_mapq\":";
    append_u64(output, combined_union.duration_ns);
    output += "}";
    output += ",\"merged_interval_counts\":{\"cigar\":";
    append_u64(output, cigar_union.merged_interval_count);
    output += ",\"mapq\":";
    append_u64(output, mapq_union.merged_interval_count);
    output += ",\"cigar_or_mapq\":";
    append_u64(output, combined_union.merged_interval_count);
    output += "}";
    output += ",\"validity\":{\"complete\":";
    append_bool(output, operation.complete);
    output += ",\"cleanup_complete\":";
    append_bool(output, operation.cleanup_complete);
    output += ",\"split_io_valid\":";
    append_bool(output, split_io_valid);
    output += ",\"clock_valid\":";
    append_bool(output, clock_valid);
    output += ",\"buffer_valid\":";
    append_bool(output, buffer_valid);
    output += ",\"buffer_overflow\":";
    append_bool(output, g_runtime.buffer_overflow.load(std::memory_order_acquire));
    output += ",\"intervals_valid\":";
    append_bool(output, intervals_valid);
    output += ",\"arithmetic_valid\":";
    append_bool(output, arithmetic_valid);
    output += ",\"capacity_contract_valid\":";
    append_bool(output, capacity_contract_valid);
    output += ",\"query_count_valid\":";
    append_bool(output, query_count_valid);
    output += ",\"index_part_count_valid\":";
    append_bool(output, index_part_count_valid);
    output += ",\"phase_count_contract_valid\":";
    append_bool(output, phase_count_contract_valid);
    output += ",\"workload_valid\":";
    append_bool(output, workload_valid);
    output += ",\"late_event\":";
    append_bool(output, late_event);
    output += ",\"valid\":";
    append_bool(output, valid);
    output += "}";
    output += ",\"artifacts\":{\"parent_cleanup_root\":";
    append_json_string(output, operation.parent_cleanup_root);
    output += ",\"split_temp_directory\":";
    append_json_string(output, operation.split_temp_directory);
    output += ",\"split_prefix\":";
    append_json_string(output, operation.split_prefix);
    output += "}";
    output += ",\"checksum\":{\"algorithm\":";
    append_json_string(output, k_checksum_algorithm);
    output += ",\"canonicalization\":";
    append_json_string(output, k_checksum_canonicalization);
    output += ",\"offset_basis_hex\":\"cbf29ce484222325\"";
    output += ",\"prime_hex\":\"00000100000001b3\"";
    output += ",\"byte_order\":\"little_endian\",\"string_encoding\":";
    append_json_string(output, "u64le_byte_length_then_exact_bytes");
    output += ",\"field_order\":[\"canonicalization_domain:string\",";
    output += "\"schema:string\",\"schema_version:u64\",\"run_token:string\",";
    output += "\"mode:string\",\"pid:u64\",\"counts:u64[]\",\"raw_records[]\"]";
    output += ",\"count_order\":[\"begin_calls\",\"end_calls\",";
    output += "\"cigar_begin_calls\",\"mapq_begin_calls\",\"claimed_slots\",";
    output += "\"recorded_intervals\",\"dropped_intervals\",\"cigar_intervals\",";
    output += "\"mapq_intervals\",\"threads_observed\",\"expected_queries\",";
    output += "\"observed_queries\",\"expected_index_parts\",";
    output += "\"observed_index_parts\"]";
    output += ",\"raw_record_order\":[\"phase_code:u32\",\"begin_ns:u64\",";
    output += "\"end_ns:u64\",\"thread_ordinal:u32\"]";
    output += ",\"raw_sort_order\":[\"begin_ns\",\"end_ns\",\"phase_code\",";
    output += "\"thread_ordinal\"]";
    output += ",\"phase_codes\":{\"cigar\":1,\"mapq\":2}";
    output += ",\"value_hex\":";
    append_hex_u64(output, checksum);
    output += "}";
    output += ",\"raw_intervals\":[";
    for (std::size_t index = 0; index < raw.size(); ++index) {
        if (index != 0)
            output += ',';
        const auto& interval = raw[index];
        output += "{\"phase\":";
        append_json_string(output, phase_name(interval.phase));
        output += ",\"begin_ns\":";
        append_u64(output, interval.begin_ns);
        output += ",\"end_ns\":";
        append_u64(output, interval.end_ns);
        output += ",\"thread_ordinal\":";
        append_u64(output, interval.thread_ordinal);
        output += '}';
    }
    output += "]}\n";
    return {.json = std::move(output), .valid = valid};
}

[[nodiscard]] bool write_all(int descriptor, std::string_view data) noexcept {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written = ::write(descriptor, data.data() + offset, data.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

struct CliOptions {
    int threads{3};
    int k{15};
    int w{10};
    int receipt_fd{-1};
    std::uint64_t expected_queries{};
    std::uint64_t expected_index_parts{};
    bool cigar{};
    bool secondary_no{};
    bool has_mode{};
    bool has_expected_queries{};
    bool has_expected_index_parts{};
    RunMode mode{};
    std::string index_output;
    std::string run_token;
    std::string parent_cleanup_root;
    std::vector<std::string_view> positional;
    std::string error;
};

[[nodiscard]] bool parse_positive_int(std::string_view value, int& result) {
    int parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
        parsed <= 0) {
        return false;
    }
    result = parsed;
    return true;
}

[[nodiscard]] bool parse_nonnegative_int(std::string_view value, int& result) {
    int parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
        parsed < 0) {
        return false;
    }
    result = parsed;
    return true;
}

[[nodiscard]] bool parse_nonnegative_u64(std::string_view value, std::uint64_t& result) {
    std::uint64_t parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size()) {
        return false;
    }
    result = parsed;
    return true;
}

[[nodiscard]] bool parse_mode(std::string_view value, RunMode& mode) {
    if (value == "index_setup")
        mode = RunMode::index_setup;
    else if (value == "no_index")
        mode = RunMode::no_index;
    else if (value == "prebuilt_index")
        mode = RunMode::prebuilt_index;
    else
        return false;
    return true;
}

[[nodiscard]] bool token_valid(std::string_view token) noexcept {
    if (token.empty() || token.size() > 128)
        return false;
    for (const unsigned char byte : token) {
        const bool allowed = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                             (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                             byte == '.' || byte == ':';
        if (!allowed)
            return false;
    }
    return true;
}

[[nodiscard]] bool private_temp_root_valid(const std::string& path) noexcept {
    if (path.empty() || path.size() > 4096 || path.front() != '/') {
        return false;
    }
    for (const unsigned char byte : path) {
        const bool allowed = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                             (byte >= '0' && byte <= '9') || byte == '/' || byte == '-' ||
                             byte == '_' || byte == '.';
        if (!allowed) {
            return false;
        }
    }

    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
           status.st_uid == ::geteuid() && (status.st_mode & (S_IRWXG | S_IRWXO)) == 0 &&
           ::access(path.c_str(), W_OK | X_OK) == 0;
}

[[nodiscard]] bool same_fd_destination(int lhs, int rhs) noexcept {
    struct stat lhs_status{};
    struct stat rhs_status{};
    if (::fstat(lhs, &lhs_status) != 0 || ::fstat(rhs, &rhs_status) != 0) {
        return false;
    }
    return lhs_status.st_dev == rhs_status.st_dev && lhs_status.st_ino == rhs_status.st_ino &&
           lhs_status.st_rdev == rhs_status.st_rdev &&
           (lhs_status.st_mode & S_IFMT) == (rhs_status.st_mode & S_IFMT);
}

[[nodiscard]] bool descriptor_alias_path(std::string_view path) noexcept {
    if (path.empty() || path == "-") {
        return true;
    }

    std::array<std::string_view, 2> leading_components{};
    std::size_t leading_count = 0;
    for (std::size_t cursor = 0; cursor < path.size();) {
        while (cursor < path.size() && path[cursor] == '/')
            ++cursor;
        const std::size_t begin = cursor;
        while (cursor < path.size() && path[cursor] != '/')
            ++cursor;
        const auto component = path.substr(begin, cursor - begin);
        if (component.empty() || component == ".")
            continue;
        // Refuse lexical ambiguity instead of trying to normalize through a
        // directory that may itself be replaced between checks.
        if (component == "..")
            return true;
        if (leading_count < leading_components.size())
            leading_components[leading_count++] = component;
    }

    if (leading_count == 0)
        return true;
    if (leading_components[0] == "proc")
        return true;
    return leading_count >= 2 && leading_components[0] == "dev" &&
           (leading_components[1] == "fd" || leading_components[1] == "stdin" ||
            leading_components[1] == "stdout" || leading_components[1] == "stderr");
}

[[nodiscard]] bool fd_destinations_proven_distinct(int lhs, int rhs) noexcept {
    struct stat lhs_status{};
    struct stat rhs_status{};
    return ::fstat(lhs, &lhs_status) == 0 && ::fstat(rhs, &rhs_status) == 0 &&
           !same_fd_destination(lhs, rhs);
}

[[nodiscard]] bool index_output_descriptors_valid(int output_fd,
                                                  int input_fd,
                                                  int receipt_fd) noexcept {
    struct stat output_status{};
    if (output_fd <= STDERR_FILENO || input_fd < 0 ||
        ::fstat(output_fd, &output_status) != 0 || !S_ISREG(output_status.st_mode)) {
        return false;
    }
    return fd_destinations_proven_distinct(output_fd, input_fd) &&
           fd_destinations_proven_distinct(output_fd, receipt_fd) &&
           fd_destinations_proven_distinct(output_fd, STDOUT_FILENO) &&
           fd_destinations_proven_distinct(output_fd, STDERR_FILENO);
}

[[nodiscard]] bool establish_blocking_regular_index_output(int output_fd) noexcept {
    struct stat output_status{};
    if (::fstat(output_fd, &output_status) != 0 || !S_ISREG(output_status.st_mode)) {
        return false;
    }

    int flags = -1;
    do {
        flags = ::fcntl(output_fd, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0 || (flags & O_ACCMODE) != O_WRONLY || (flags & O_APPEND) != 0) {
        return false;
    }

    int mode_status = 0;
    if ((flags & O_NONBLOCK) != 0) {
        do {
            mode_status = ::fcntl(output_fd, F_SETFL, flags & ~O_NONBLOCK);
        } while (mode_status != 0 && errno == EINTR);
        if (mode_status != 0) {
            return false;
        }
    }

    int verified_flags = -1;
    do {
        verified_flags = ::fcntl(output_fd, F_GETFL);
    } while (verified_flags < 0 && errno == EINTR);
    return verified_flags >= 0 && (verified_flags & O_ACCMODE) == O_WRONLY &&
           (verified_flags & (O_APPEND | O_NONBLOCK)) == 0;
}

class OwnedDescriptor final {
  public:
    explicit OwnedDescriptor(int descriptor = -1) noexcept : descriptor_(descriptor) {}

    ~OwnedDescriptor() noexcept {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }

    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

  private:
    int descriptor_;
};

[[nodiscard]] int open_index_input_descriptor(std::string_view path) noexcept {
    if (path == "-") {
#if defined(F_DUPFD_CLOEXEC)
        return ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
        const int descriptor = ::fcntl(STDIN_FILENO, F_DUPFD, STDERR_FILENO + 1);
        if (descriptor >= 0)
            (void)::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
        return descriptor;
#endif
    }
    int flags = O_RDONLY;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    int descriptor = -1;
    do {
        descriptor = ::open(path.data(), flags);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

[[nodiscard]] int open_index_output_descriptor(std::string_view path) noexcept {
    int flags = O_WRONLY | O_CREAT | O_NONBLOCK;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    int descriptor = -1;
    do {
        // Deliberately omit O_TRUNC. The opened object is truncated only after
        // every descriptor identity comparison has succeeded.
        descriptor = ::open(path.data(), flags, 0666);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

[[nodiscard]] bool truncate_and_rewind_index_output(int descriptor) noexcept {
    int truncate_status = -1;
    do {
        truncate_status = ::ftruncate(descriptor, 0);
    } while (truncate_status != 0 && errno == EINTR);
    if (truncate_status != 0)
        return false;
    return ::lseek(descriptor, 0, SEEK_SET) == 0;
}

[[nodiscard]] bool receipt_fd_valid(int descriptor) noexcept {
    if (descriptor <= STDERR_FILENO || ::fcntl(descriptor, F_GETFD) == -1) {
        return false;
    }
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags == -1 || (flags & O_ACCMODE) == O_RDONLY) {
        return false;
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0) {
        return false;
    }
    if (S_ISREG(status.st_mode) && (status.st_size != 0 || ::lseek(descriptor, 0, SEEK_CUR) != 0)) {
        return false;
    }
    return !same_fd_destination(descriptor, STDOUT_FILENO) &&
           !same_fd_destination(descriptor, STDERR_FILENO);
}

[[nodiscard]] CliOptions parse_cli(int argc, char** argv) {
    CliOptions options;
    bool positional_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto following = [&](std::string_view option) -> std::string_view {
            if (index + 1 >= argc) {
                options.error = std::string(option) + " requires a value";
                return {};
            }
            return argv[++index];
        };

        if (positional_only) {
            options.positional.push_back(argument);
        } else if (argument == "--") {
            positional_only = true;
        } else if (argument == "-c") {
            options.cigar = true;
        } else if (argument == "--secondary=no") {
            options.secondary_no = true;
        } else if (argument == "-t" || argument == "-k" || argument == "-w" || argument == "-d") {
            const auto value = following(argument);
            if (!options.error.empty())
                break;
            if (argument == "-d")
                options.index_output = value;
            else {
                int parsed = 0;
                if (!parse_positive_int(value, parsed)) {
                    options.error = std::string(argument) + " requires a positive integer";
                    break;
                }
                if (argument == "-t")
                    options.threads = parsed;
                if (argument == "-k")
                    options.k = parsed;
                if (argument == "-w")
                    options.w = parsed;
            }
        } else if (argument.size() > 2 &&
                   (argument.starts_with("-t") || argument.starts_with("-k") ||
                    argument.starts_with("-w") || argument.starts_with("-d"))) {
            const auto option = argument.substr(0, 2);
            const auto value = argument.substr(2);
            if (option == "-d")
                options.index_output = value;
            else {
                int parsed = 0;
                if (!parse_positive_int(value, parsed)) {
                    options.error = std::string(option) + " requires a positive integer";
                    break;
                }
                if (option == "-t")
                    options.threads = parsed;
                if (option == "-k")
                    options.k = parsed;
                if (option == "-w")
                    options.w = parsed;
            }
        } else if (argument == "--bioseq-e2e-receipt-fd") {
            const auto value = following(argument);
            if (!options.error.empty())
                break;
            if (!parse_nonnegative_int(value, options.receipt_fd)) {
                options.error = "--bioseq-e2e-receipt-fd requires an integer";
                break;
            }
        } else if (argument.starts_with("--bioseq-e2e-receipt-fd=")) {
            if (!parse_nonnegative_int(argument.substr(std::strlen("--bioseq-e2e-receipt-fd=")),
                                       options.receipt_fd)) {
                options.error = "--bioseq-e2e-receipt-fd requires an integer";
                break;
            }
        } else if (argument == "--bioseq-e2e-expected-queries") {
            const auto value = following(argument);
            if (!options.error.empty())
                break;
            if (!parse_nonnegative_u64(value, options.expected_queries)) {
                options.error = "--bioseq-e2e-expected-queries requires a nonnegative integer";
                break;
            }
            options.has_expected_queries = true;
        } else if (argument.starts_with("--bioseq-e2e-expected-queries=")) {
            if (!parse_nonnegative_u64(
                    argument.substr(std::strlen("--bioseq-e2e-expected-queries=")),
                    options.expected_queries)) {
                options.error = "--bioseq-e2e-expected-queries requires a nonnegative integer";
                break;
            }
            options.has_expected_queries = true;
        } else if (argument == "--bioseq-e2e-expected-index-parts") {
            const auto value = following(argument);
            if (!options.error.empty())
                break;
            if (!parse_nonnegative_u64(value, options.expected_index_parts)) {
                options.error =
                    "--bioseq-e2e-expected-index-parts requires a positive integer";
                break;
            }
            options.has_expected_index_parts = true;
        } else if (argument.starts_with("--bioseq-e2e-expected-index-parts=")) {
            if (!parse_nonnegative_u64(
                    argument.substr(std::strlen("--bioseq-e2e-expected-index-parts=")),
                    options.expected_index_parts)) {
                options.error =
                    "--bioseq-e2e-expected-index-parts requires a positive integer";
                break;
            }
            options.has_expected_index_parts = true;
        } else if (argument == "--bioseq-e2e-temp-root") {
            options.parent_cleanup_root = following(argument);
            if (!options.error.empty())
                break;
        } else if (argument.starts_with("--bioseq-e2e-temp-root=")) {
            options.parent_cleanup_root =
                argument.substr(std::strlen("--bioseq-e2e-temp-root="));
        } else if (argument == "--bioseq-e2e-run-token") {
            options.run_token = following(argument);
            if (!options.error.empty())
                break;
        } else if (argument.starts_with("--bioseq-e2e-run-token=")) {
            options.run_token = argument.substr(std::strlen("--bioseq-e2e-run-token="));
        } else if (argument == "--bioseq-e2e-mode") {
            const auto value = following(argument);
            if (!options.error.empty())
                break;
            if (!parse_mode(value, options.mode)) {
                options.error = "invalid --bioseq-e2e-mode";
                break;
            }
            options.has_mode = true;
        } else if (argument.starts_with("--bioseq-e2e-mode=")) {
            if (!parse_mode(argument.substr(std::strlen("--bioseq-e2e-mode=")), options.mode)) {
                options.error = "invalid --bioseq-e2e-mode";
                break;
            }
            options.has_mode = true;
        } else if (!argument.empty() && argument.front() == '-') {
            options.error = "unsupported option: " + std::string(argument);
            break;
        } else {
            options.positional.push_back(argument);
        }
    }

    if (!options.error.empty())
        return options;
    if (!options.has_mode) {
        options.error = "--bioseq-e2e-mode is required";
    } else if (!receipt_fd_valid(options.receipt_fd)) {
        options.error = "receipt descriptor must be dedicated, open, and writable";
    } else if (!token_valid(options.run_token)) {
        options.error = "run token must be 1..128 safe ASCII characters";
    } else if (!options.has_expected_queries) {
        options.error = "--bioseq-e2e-expected-queries is required";
    } else if (!options.has_expected_index_parts || options.expected_index_parts == 0) {
        options.error = "--bioseq-e2e-expected-index-parts must be positive";
    } else if (const auto expected = expected_phase_counts(options.mode,
                                                           options.expected_queries,
                                                           options.expected_index_parts);
               !expected.arithmetic_valid ||
               expected.total > static_cast<std::uint64_t>(k_interval_capacity)) {
        options.error = "expected phase interval count exceeds receipt capacity";
    } else if (!options.parent_cleanup_root.empty() &&
               !private_temp_root_valid(options.parent_cleanup_root)) {
        options.error = "--bioseq-e2e-temp-root must name a private owner-only directory";
    } else if (options.k > 28) {
        options.error = "-k must be at most 28";
    } else if (options.w > std::numeric_limits<short>::max()) {
        options.error = "-w exceeds minimap2's index-option range";
    } else if (options.mode == RunMode::index_setup) {
        if (options.expected_queries != 0 || options.index_output.empty() ||
            options.positional.size() != 1 || options.cigar || options.secondary_no) {
            options.error =
                "index_setup requires expected queries 0, -d OUTPUT, and one target input";
        } else if (descriptor_alias_path(options.index_output)) {
            options.error = "index output must not name a descriptor pseudo-path";
        }
    } else if (options.expected_queries == 0 ||
               options.parent_cleanup_root.empty() ||
               !options.index_output.empty() || options.positional.size() != 2 ||
               !options.cigar || !options.secondary_no) {
        options.error = "mapping requires positive expected queries, a private temp root, "
                        "and -c --secondary=no TARGET QUERY";
    }
    return options;
}

[[nodiscard]] bool initialize_minimap_options(const CliOptions& cli,
                                              mm_idxopt_t& index_options,
                                              mm_mapopt_t& map_options) noexcept {
    if (mm_set_opt(nullptr, &index_options, &map_options) != 0)
        return false;
    index_options.k = static_cast<short>(cli.k);
    index_options.w = static_cast<short>(cli.w);
    if (cli.mode != RunMode::index_setup)
        map_options.flag |= MM_F_CIGAR | MM_F_OUT_CG | MM_F_NO_PRINT_2ND;
    return mm_check_opt(&index_options, &map_options) == 0;
}

[[nodiscard]] bool cleanup_split_workspace(OperationResult& result,
                                           int part_count,
                                           bool remove_part_files) noexcept {
    if (result.split_temp_directory.empty()) {
        result.cleanup_complete = true;
        return true;
    }
    if (remove_part_files && part_count > 0) {
        mm_split_rm_tmp(result.split_prefix.c_str(), part_count);
    }

    int remove_status = -1;
    do {
        remove_status = ::rmdir(result.split_temp_directory.c_str());
    } while (remove_status != 0 && errno == EINTR);
    result.cleanup_complete = remove_status == 0;
    return result.cleanup_complete;
}

[[nodiscard]] OperationResult run_index_setup(const CliOptions& cli,
                                              const mm_idxopt_t& index_options) noexcept {
    OperationResult result{.parent_cleanup_root = cli.parent_cleanup_root};
    // Keep this defensive check local as well as in parse_cli: no vendored
    // reader/open operation may precede lexical pseudo-path rejection.
    if (descriptor_alias_path(cli.index_output)) {
        std::fprintf(stderr,
                     "[bioseq-minimap2-e2e] index output must not name a descriptor "
                     "pseudo-path\n");
        return result;
    }

    OwnedDescriptor input{
        open_index_input_descriptor(cli.positional[0])};
    if (input.get() < 0) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] cannot open target\n");
        return result;
    }
    OwnedDescriptor output{
        open_index_output_descriptor(cli.index_output)};
    if (output.get() < 0) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] cannot open index output\n");
        return result;
    }
    if (!index_output_descriptors_valid(output.get(), input.get(), cli.receipt_fd)) {
        std::fprintf(stderr,
                     "[bioseq-minimap2-e2e] index output must use a dedicated destination\n");
        return result;
    }
    if (!establish_blocking_regular_index_output(output.get())) {
        std::fprintf(stderr,
                     "[bioseq-minimap2-e2e] cannot establish regular index output mode\n");
        return result;
    }
    if (!truncate_and_rewind_index_output(output.get())) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] cannot prepare index output\n");
        return result;
    }

    // The private reader consumes both descriptors on every return path.
    auto* reader =
        mm_idx_reader_open_fd(input.release(), &index_options, output.release());
    if (reader == nullptr) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] cannot open target or index output\n");
        return result;
    }

    std::uint64_t parts = 0;
    while (auto* index = mm_idx_reader_read(reader, cli.threads)) {
        ++parts;
        mm_idx_destroy(index);
    }
    result.observed_index_parts = parts;
    const bool reader_at_eof = mm_idx_reader_eof(reader);
    FILE* const index_output = reader->fp_out;
    const bool output_flushed = std::fflush(index_output) == 0;
    const bool output_error_free = std::ferror(index_output) == 0;
    reader->fp_out = nullptr;
    const bool output_closed = std::fclose(index_output) == 0;
    mm_idx_reader_close(reader);
    result.complete = parts > 0 && reader_at_eof && output_flushed && output_error_free &&
                      output_closed;
    if (!result.complete) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] index construction failed\n");
    }
    return result;
}

[[nodiscard]] OperationResult run_mapping(const CliOptions& cli,
                                          const mm_idxopt_t& index_options,
                                          const mm_mapopt_t& base_map_options) noexcept {
    OperationResult result{.parent_cleanup_root = cli.parent_cleanup_root};
    auto* reader = mm_idx_reader_open(cli.positional[0].data(), &index_options, nullptr);
    if (reader == nullptr || (reader->is_idx && reader->fp.idx == nullptr) ||
        (!reader->is_idx && reader->fp.seq == nullptr)) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] cannot open target\n");
        if (reader != nullptr)
            std::free(reader);
        return result;
    }

    const bool is_prebuilt = reader->is_idx != 0;
    if ((cli.mode == RunMode::prebuilt_index) != is_prebuilt) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] target does not match declared mode\n");
        mm_idx_reader_close(reader);
        return result;
    }
    auto* index = mm_idx_reader_read(reader, cli.threads);
    if (index == nullptr) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] target has no index part\n");
        mm_idx_reader_close(reader);
        return result;
    }
    const bool multipart = !mm_idx_reader_eof(reader);
    if (multipart) {
        result.split_temp_directory = cli.parent_cleanup_root;
        if (result.split_temp_directory.back() != '/')
            result.split_temp_directory += '/';
        result.split_temp_directory += "bioseq-minimap2-e2e-split.XXXXXX";
        if (::mkdtemp(result.split_temp_directory.data()) == nullptr) {
            std::fprintf(stderr, "[bioseq-minimap2-e2e] cannot create private split directory\n");
            result.split_temp_directory.clear();
            mm_idx_destroy(index);
            mm_idx_reader_close(reader);
            return result;
        }
        result.split_prefix = result.split_temp_directory + "/part";
    }

    int mapped_parts = 0;
    bool mapping_ok = true;
    auto map_options = base_map_options;
    if (multipart)
        map_options.split_prefix = result.split_prefix.c_str();
    do {
        mm_mapopt_update(&map_options, index);
        mapping_ok = mm_map_file(index, cli.positional[1].data(), &map_options, cli.threads) == 0;
        mm_idx_destroy(index);
        index = nullptr;
        if (!mapping_ok)
            break;
        ++mapped_parts;
        index = mm_idx_reader_read(reader, cli.threads);
    } while (index != nullptr);

    if (index != nullptr)
        mm_idx_destroy(index);
    const bool reader_at_eof = mm_idx_reader_eof(reader);
    result.observed_index_parts = static_cast<std::uint64_t>(reader->n_parts);
    mm_idx_reader_close(reader);
    if (!mapping_ok || !reader_at_eof) {
        const bool cleanup_ok = !multipart ||
            cleanup_split_workspace(result, mapped_parts + 1, true);
        std::fprintf(stderr, "[bioseq-minimap2-e2e] mapping an index part failed\n");
        result.complete = false;
        if (!cleanup_ok)
            std::fprintf(stderr, "[bioseq-minimap2-e2e] split cleanup incomplete\n");
        return result;
    }

    if (multipart) {
        const char* query_path = cli.positional[1].data();
        if (mm_split_merge(1, &query_path, &map_options, mapped_parts) != 0) {
            const bool cleanup_ok = cleanup_split_workspace(result, mapped_parts, true);
            std::fprintf(stderr, "[bioseq-minimap2-e2e] split-index merge failed\n");
            result.complete = false;
            if (!cleanup_ok)
                std::fprintf(stderr, "[bioseq-minimap2-e2e] split cleanup incomplete\n");
            return result;
        }
        if (!cleanup_split_workspace(result, mapped_parts, true)) {
            std::fprintf(stderr, "[bioseq-minimap2-e2e] split cleanup incomplete\n");
            result.complete = false;
            return result;
        }
    }
    result.complete = true;
    return result;
}

void print_usage(const char* program) noexcept {
    std::fprintf(stderr,
                 "usage: %s --bioseq-e2e-mode MODE --bioseq-e2e-run-token TOKEN "
                 "--bioseq-e2e-receipt-fd FD --bioseq-e2e-expected-queries N "
                 "--bioseq-e2e-expected-index-parts N "
                 "[--bioseq-e2e-temp-root PRIVATE_DIR] [-t N] [-k N] [-w N] "
                 "{-d INDEX TARGET | -c --secondary=no TARGET QUERY}\n",
                 program);
}

} // namespace

extern "C" std::uint64_t bioseq_minimap2_e2e_phase_begin(std::uint32_t phase) {
    if (!g_runtime.active.load(std::memory_order_acquire))
        return 0;
    g_runtime.begin_calls.fetch_add(1, std::memory_order_relaxed);
    if (phase == k_phase_cigar) {
        g_runtime.cigar_begin_calls.fetch_add(1, std::memory_order_relaxed);
    } else if (phase == k_phase_mapq) {
        g_runtime.mapq_begin_calls.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_runtime.interval_valid.store(false, std::memory_order_release);
    }

    std::uint64_t begin_ns = 0;
    if (!monotonic_now_ns(begin_ns)) {
        g_runtime.clock_valid.store(false, std::memory_order_release);
        return 0;
    }
    return begin_ns;
}

extern "C" void bioseq_minimap2_e2e_phase_end(std::uint32_t phase, std::uint64_t begin_ns) {
    g_runtime.end_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_runtime.active.load(std::memory_order_acquire)) {
        if (begin_ns != 0) {
            g_runtime.late_event.store(true, std::memory_order_release);
        }
        return;
    }
    if (begin_ns == 0)
        return;

    std::uint64_t end_ns = 0;
    if (!monotonic_now_ns(end_ns)) {
        g_runtime.clock_valid.store(false, std::memory_order_release);
        return;
    }
    if (end_ns < begin_ns || (phase != k_phase_cigar && phase != k_phase_mapq)) {
        g_runtime.interval_valid.store(false, std::memory_order_release);
        return;
    }

    const auto slot_index = g_runtime.claimed_slots.fetch_add(1, std::memory_order_relaxed);
    if (slot_index >= k_interval_capacity) {
        g_runtime.buffer_overflow.store(true, std::memory_order_release);
        return;
    }
    auto& slot = g_runtime.slots[static_cast<std::size_t>(slot_index)];
    slot.interval = RawInterval{
        .begin_ns = begin_ns,
        .end_ns = end_ns,
        .phase = phase,
        .thread_ordinal = current_thread_ordinal(),
    };
    slot.committed.store(1, std::memory_order_release);
}

extern "C" void bioseq_minimap2_e2e_io_error_reset() {
    g_runtime.split_io_error.store(false, std::memory_order_release);
}

extern "C" void bioseq_minimap2_e2e_io_error_mark() {
    g_runtime.split_io_error.store(true, std::memory_order_release);
}

extern "C" int bioseq_minimap2_e2e_io_error_seen() {
    return g_runtime.split_io_error.load(std::memory_order_acquire) ? 1 : 0;
}

int main(int argc, char** argv) {
    const auto cli = parse_cli(argc, argv);
    if (!cli.error.empty()) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] %s\n", cli.error.c_str());
        print_usage(argv[0]);
        return 64;
    }
    if (!runtime_start()) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] receipt runtime already started\n");
        return 70;
    }

    ::signal(SIGPIPE, SIG_IGN);
    mm_verbose = 1;
    mm_idxopt_t index_options{};
    mm_mapopt_t map_options{};
    OperationResult operation{.parent_cleanup_root = cli.parent_cleanup_root};
    const bool options_ok = initialize_minimap_options(cli, index_options, map_options);
    if (!options_ok) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] invalid minimap2 options\n");
    } else if (cli.mode == RunMode::index_setup) {
        operation = run_index_setup(cli, index_options);
    } else {
        operation = run_mapping(cli, index_options, map_options);
    }

    if (std::fflush(stdout) != 0) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] failed to flush PAF output\n");
        operation.complete = false;
    }
    const auto receipt = make_receipt(cli.run_token,
                                      cli.mode,
                                      cli.expected_queries,
                                      cli.expected_index_parts,
                                      operation);
    const bool receipt_written = write_all(cli.receipt_fd, receipt.json);
    const bool receipt_closed = ::close(cli.receipt_fd) == 0;
    if (!receipt_written || !receipt_closed) {
        std::fprintf(stderr, "[bioseq-minimap2-e2e] failed to deliver phase receipt\n");
        return 74;
    }
    return receipt.valid ? 0 : 1;
}
