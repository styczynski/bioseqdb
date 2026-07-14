#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

import bioseq.cache.core;

using namespace bioseq::cache;

namespace {

using Clock = std::chrono::steady_clock;

struct BenchChecksums {
    [[nodiscard]] std::expected<std::uint32_t, ChecksumError> block_crc32c(
        std::span<const std::byte> bytes) const noexcept {
        std::uint32_t hash = 2'166'136'261u;
        for (const std::byte byte : bytes) {
            hash ^= std::to_integer<std::uint8_t>(byte);
            hash *= 16'777'619u;
        }
        return hash;
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError> sha256(
        std::span<const std::byte> bytes) const noexcept {
        std::array<std::uint64_t, 4> lanes{
            0xcbf29ce484222325ULL,
            0x9e3779b97f4a7c15ULL,
            0x6a09e667f3bcc909ULL,
            0xbb67ae8584caa73bULL,
        };
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const std::uint64_t value =
                std::to_integer<std::uint8_t>(bytes[index]);
            for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
                lanes[lane] ^= value + index + lane * 0x9d;
                lanes[lane] *= 0x100000001b3ULL + lane * 2u;
                lanes[lane] ^= lanes[lane] >> (17u + lane);
            }
        }
        CacheDigest256 digest{};
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
            for (std::size_t byte = 0; byte < 8; ++byte) {
                digest.bytes[lane * 8u + byte] = static_cast<std::uint8_t>(
                    lanes[lane] >> (byte * 8u));
            }
        }
        return digest;
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError>
    manifest_sha256(
        std::span<const CacheDigest256> block_digests) const noexcept {
        std::vector<std::byte> bytes;
        bytes.reserve(block_digests.size() * 32u);
        for (const CacheDigest256& digest : block_digests) {
            for (const std::uint8_t byte : digest.bytes) {
                bytes.push_back(static_cast<std::byte>(byte));
            }
        }
        return sha256(bytes);
    }
};

struct BenchCounter {
    std::uint64_t value = 0;

    [[nodiscard]] std::uint64_t load() const noexcept {
        return value;
    }

    bool compare_exchange(
        std::uint64_t& expected,
        std::uint64_t desired) noexcept {
        if (value != expected) {
            expected = value;
            return false;
        }
        value = desired;
        return true;
    }

    void subtract(std::uint64_t bytes) noexcept {
        value -= bytes;
    }
};

struct BenchBudget {
    [[nodiscard]] std::uint64_t allocatable_bytes() const noexcept {
        return (std::numeric_limits<std::uint64_t>::max)();
    }

    [[nodiscard]] std::uint64_t total_bytes() const noexcept {
        return 8ULL << 30;
    }

    [[nodiscard]] bool under_pressure() const noexcept {
        return false;
    }
};

constexpr ColumnStoreIdentity kIdentity{
    .postgres_major = 18,
    .boot_nonce = 0x1111'2222'3333'4444ULL,
    .database_oid = 1,
    .owner_role_oid = 2,
    .datum_width = sizeof(std::uintptr_t),
};

std::array<ColumnSchema, 3> benchmark_schema() {
    return {{
        {
            .name = "accepted",
            .descriptor = {
                .type_oid = 16,
                .typmod = -1,
                .type_length = 1,
                .by_value = 1,
                .type_alignment = 1,
                .encoding = ColumnEncoding::BooleanBits,
            },
            .semantic_id = {.low = 1},
        },
        {
            .name = "score",
            .descriptor = {
                .type_oid = 23,
                .typmod = -1,
                .type_length = 4,
                .by_value = 1,
                .type_alignment = 4,
                .encoding = ColumnEncoding::FixedByValue,
            },
            .semantic_id = {.low = 2},
        },
        {
            .name = "payload",
            .descriptor = {
                .type_oid = 17,
                .typmod = -1,
                .type_length = -1,
                .type_alignment = 1,
                .encoding = ColumnEncoding::BinarySend,
                .send_function_oid = 1,
                .receive_function_oid = 2,
            },
            .semantic_id = {.low = 3},
        },
    }};
}

std::expected<std::vector<std::byte>, FormatError> make_blob(
    std::uint64_t row_count,
    BenchChecksums& checksums) {
    auto schema = benchmark_schema();
    ColumnStoreBuilder builder(kIdentity, schema);
    for (std::uint64_t row = 0; row < row_count; ++row) {
        const std::array<std::byte, 1> boolean{
            static_cast<std::byte>(row & 1u)};
        const std::uint32_t score = static_cast<std::uint32_t>(row);
        const std::array<std::byte, 4> fixed{
            static_cast<std::byte>(score & 0xffu),
            static_cast<std::byte>((score >> 8u) & 0xffu),
            static_cast<std::byte>((score >> 16u) & 0xffu),
            static_cast<std::byte>((score >> 24u) & 0xffu),
        };
        std::array<std::byte, 16> payload{};
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<std::byte>(row + index);
        }
        const std::array<EncodedColumnValue, 3> values{{
            {.bytes = boolean},
            {.bytes = fixed},
            {.bytes = payload},
        }};
        const auto appended = builder.append_row(values);
        if (!appended.has_value()) {
            return std::unexpected(appended.error());
        }
    }
    return builder.seal(checksums);
}

std::uint64_t percentile(
    std::vector<std::uint64_t> samples,
    std::uint64_t numerator,
    std::uint64_t denominator) {
    std::sort(samples.begin(), samples.end());
    const std::uint64_t rank =
        (numerator * samples.size() + denominator - 1u) / denominator;
    return samples[static_cast<std::size_t>(std::max<std::uint64_t>(1, rank) - 1u)];
}

void append_samples_json(
    std::ostringstream& output,
    std::span<const std::uint64_t> samples) {
    output << '[';
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << samples[index];
    }
    output << ']';
}

std::uint64_t mix_byte(std::uint64_t hash, std::uint8_t byte) noexcept {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
    return hash;
}

std::uint64_t expected_row_digest(std::uint64_t rows) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::uint64_t row = 0; row < rows; ++row) {
        hash = mix_byte(hash, static_cast<std::uint8_t>(row & 1u));
        const std::uint32_t score = static_cast<std::uint32_t>(row);
        for (std::size_t byte = 0; byte < 4; ++byte) {
            hash = mix_byte(hash, static_cast<std::uint8_t>(
                score >> (byte * 8u)));
        }
        for (std::size_t byte = 0; byte < 16; ++byte) {
            hash = mix_byte(hash, static_cast<std::uint8_t>(row + byte));
        }
    }
    return hash;
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::string hex_digest(const CacheDigest256& digest) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint8_t byte : digest.bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

struct Options {
    std::optional<std::string> output;
    std::uint64_t rows = 4'096;
    std::uint32_t samples = 7;
};

template <class Integer>
bool parse_positive_integer(std::string_view text, Integer& value) noexcept {
    Integer parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{}
        || result.ptr != text.data() + text.size()
        || parsed == 0) {
        return false;
    }
    value = parsed;
    return true;
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument.starts_with("--output=")) {
            options.output = std::string(argument.substr(9));
        } else if (argument.starts_with("--rows=")) {
            if (!parse_positive_integer(argument.substr(7), options.rows)) {
                return std::nullopt;
            }
        } else if (argument.starts_with("--samples=")) {
            if (!parse_positive_integer(
                    argument.substr(10), options.samples)) {
                return std::nullopt;
            }
        } else if (!argument.starts_with('-') && !options.output.has_value()) {
            options.output = std::string(argument);
        } else {
            return std::nullopt;
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        std::cerr << "usage: bioseq_cache_bench [--output PATH] "
                     "[--rows=N] [--samples=N]\n";
        return 2;
    }

    BenchChecksums checksums;
    const auto schema = benchmark_schema();
    std::vector<std::uint64_t> encode_samples;
    encode_samples.reserve(options->samples);
    std::vector<std::byte> blob;
    for (std::uint32_t sample = 0; sample < options->samples; ++sample) {
        const auto started = Clock::now();
        auto encoded = make_blob(options->rows, checksums);
        const auto finished = Clock::now();
        if (!encoded.has_value()) {
            std::cerr << "column encoding failed\n";
            return 1;
        }
        blob = std::move(*encoded);
        encode_samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - started).count()));
    }

    const std::uint64_t expected_digest = expected_row_digest(options->rows);
    std::uint64_t decoded_digest = 0;
    std::size_t row_group_count = 0;
    std::vector<std::uint64_t> prevalidation_samples;
    prevalidation_samples.reserve(options->samples);
    for (std::uint32_t sample = 0; sample < options->samples; ++sample) {
        const auto started = Clock::now();
        auto view = validate_column_store(blob, kIdentity, schema, checksums);
        const auto finished = Clock::now();
        if (!view.has_value() || view->row_count() != options->rows) {
            std::cerr << "column prevalidation failed\n";
            return 1;
        }
        row_group_count = view->row_group_count();
        prevalidation_samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - started).count()));
    }

    std::vector<std::uint64_t> decode_samples;
    decode_samples.reserve(options->samples);
    for (std::uint32_t sample = 0; sample < options->samples; ++sample) {
        const auto started = Clock::now();
        auto view = validate_column_store(
            blob, kIdentity, schema, checksums);
        if (!view.has_value()) {
            std::cerr << "column validation failed\n";
            return 1;
        }
        row_group_count = view->row_group_count();
        std::uint64_t local_digest = 0xcbf29ce484222325ULL;
        for (std::uint64_t row = 0; row < view->row_count(); ++row) {
            const auto accepted = view->value(row, 0);
            const auto score = view->value(row, 1);
            const auto payload = view->value(row, 2);
            if (!accepted.has_value() || !score.has_value()
                || !payload.has_value()
                || !accepted->is_boolean || accepted->is_null
                || score->is_null || payload->is_null
                || score->bytes.size() != 4 || payload->bytes.size() != 16) {
                std::cerr << "column replay failed\n";
                return 1;
            }
            local_digest = mix_byte(
                local_digest,
                static_cast<std::uint8_t>(accepted->boolean_value));
            for (const std::byte byte : score->bytes) {
                local_digest = mix_byte(
                    local_digest, std::to_integer<std::uint8_t>(byte));
            }
            for (const std::byte byte : payload->bytes) {
                local_digest = mix_byte(
                    local_digest, std::to_integer<std::uint8_t>(byte));
            }
        }
        const auto finished = Clock::now();
        if (view->row_count() != options->rows
            || local_digest != expected_digest) {
            std::cerr << "decoded correctness digest mismatch\n";
            return 1;
        }
        decoded_digest = local_digest;
        decode_samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - started).count()));
    }

    std::vector<std::uint64_t> memory_copy_samples;
    memory_copy_samples.reserve(options->samples);
    std::vector<std::byte> copied(blob.size());
    constexpr std::uint64_t copy_repetitions = 64;
    std::uint8_t copy_guard = 0;
    for (std::uint32_t sample = 0; sample < options->samples; ++sample) {
        const auto started = Clock::now();
        for (std::uint64_t repetition = 0;
             repetition < copy_repetitions;
             ++repetition) {
            std::memcpy(copied.data(), blob.data(), blob.size());
            if (!copied.empty()) {
                copy_guard ^= std::to_integer<std::uint8_t>(
                    copied[repetition % copied.size()]);
            }
        }
        const auto finished = Clock::now();
        memory_copy_samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - started).count()));
    }

    std::vector<std::uint64_t> admission_samples;
    admission_samples.reserve(options->samples);
    constexpr std::uint64_t admission_operations = 100'000;
    for (std::uint32_t sample = 0; sample < options->samples; ++sample) {
        BenchCounter counter;
        const BenchBudget budget;
        BasicCacheAdmission admission(counter, budget);
        const auto started = Clock::now();
        for (std::uint64_t operation = 0;
             operation < admission_operations;
             ++operation) {
            auto permit = admission.try_reserve(64);
            if (!permit.has_value()) {
                std::cerr << "admission failed\n";
                return 1;
            }
            permit->release();
        }
        const auto finished = Clock::now();
        if (counter.value != 0) {
            std::cerr << "admission permit leak\n";
            return 1;
        }
        admission_samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - started).count()));
    }

    const auto digest = checksums.sha256(blob);
    if (!digest.has_value()) {
        return 1;
    }
    const auto decode_p50 = percentile(decode_samples, 50, 100);
    const auto memory_copy_p50 = percentile(memory_copy_samples, 50, 100);
    const long double fixed_decode_bytes =
        static_cast<long double>(options->rows) * 4.0L;
    const long double copied_bytes = static_cast<long double>(blob.size())
        * static_cast<long double>(copy_repetitions);
    const std::uint64_t fixed_decode_bytes_per_second = decode_p50 == 0
        ? 0
        : static_cast<std::uint64_t>(
            fixed_decode_bytes * 1'000'000'000.0L / decode_p50);
    const std::uint64_t memory_copy_bytes_per_second = memory_copy_p50 == 0
        ? 0
        : static_cast<std::uint64_t>(
            copied_bytes * 1'000'000'000.0L / memory_copy_p50);
    std::ostringstream json;
    json << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"rows\": " << options->rows << ",\n"
         << "  \"samples\": " << options->samples << ",\n"
         << "  \"correctness\": {\n"
         << "    \"passed\": true,\n"
         << "    \"row_count\": " << options->rows << ",\n"
         << "    \"expected_ordered_checksum\": \""
         << hex_u64(expected_digest) << "\",\n"
         << "    \"actual_ordered_checksum\": \""
         << hex_u64(decoded_digest) << "\",\n"
         << "    \"column_blob_digest\": \"" << hex_digest(*digest)
         << "\",\n"
         << "    \"memory_copy_guard\": "
         << static_cast<unsigned int>(copy_guard) << ",\n"
         << "    \"admission_final_reserved_bytes\": 0\n"
         << "  },\n"
         << "  \"metrics\": {\n"
         << "    \"encoded_logical_bytes\": " << options->rows * 21u
         << ",\n"
         << "    \"encoded_stored_bytes\": " << blob.size() << ",\n"
         << "    \"row_group_count\": " << row_group_count << ",\n"
         << "    \"full_block_prevalidation_bytes\": " << blob.size()
         << ",\n"
         << "    \"full_block_prevalidation_p50_ns\": "
         << percentile(prevalidation_samples, 50, 100) << ",\n"
         << "    \"full_block_prevalidation_p95_ns\": "
         << percentile(prevalidation_samples, 95, 100) << ",\n"
         << "    \"full_block_prevalidation_p99_ns\": "
         << percentile(prevalidation_samples, 99, 100) << ",\n"
         << "    \"fixed_decode_bytes_per_second\": "
         << fixed_decode_bytes_per_second << ",\n"
         << "    \"memory_copy_bytes_per_second\": "
         << memory_copy_bytes_per_second << "\n"
         << "  },\n"
         << "  \"benchmarks\": [\n"
         << "    {\"name\": \"column_encode\", \"raw_samples_ns\": ";
    append_samples_json(json, encode_samples);
    json << ", \"p50_ns\": " << percentile(encode_samples, 50, 100)
         << ", \"p95_ns\": " << percentile(encode_samples, 95, 100)
         << ", \"p99_ns\": " << percentile(encode_samples, 99, 100)
         << ", \"operations\": " << options->rows << "},\n"
         << "    {\"name\": \"column_full_prevalidation\", "
            "\"raw_samples_ns\": ";
    append_samples_json(json, prevalidation_samples);
    json << ", \"p50_ns\": "
         << percentile(prevalidation_samples, 50, 100)
         << ", \"p95_ns\": "
         << percentile(prevalidation_samples, 95, 100)
         << ", \"p99_ns\": "
         << percentile(prevalidation_samples, 99, 100)
         << ", \"operations\": " << blob.size() << "},\n"
         << "    {\"name\": \"column_validate_decode\", "
            "\"raw_samples_ns\": ";
    append_samples_json(json, decode_samples);
    json << ", \"p50_ns\": " << decode_p50
         << ", \"p95_ns\": " << percentile(decode_samples, 95, 100)
         << ", \"p99_ns\": " << percentile(decode_samples, 99, 100)
         << ", \"operations\": " << options->rows << "},\n"
         << "    {\"name\": \"admission_reserve_release\", "
            "\"raw_samples_ns\": ";
    append_samples_json(json, admission_samples);
    json << ", \"p50_ns\": " << percentile(admission_samples, 50, 100)
         << ", \"p95_ns\": " << percentile(admission_samples, 95, 100)
         << ", \"p99_ns\": " << percentile(admission_samples, 99, 100)
         << ", \"operations\": " << admission_operations << "},\n"
         << "    {\"name\": \"memory_copy_reference\", "
            "\"raw_samples_ns\": ";
    append_samples_json(json, memory_copy_samples);
    json << ", \"p50_ns\": " << memory_copy_p50
         << ", \"p95_ns\": " << percentile(memory_copy_samples, 95, 100)
         << ", \"p99_ns\": " << percentile(memory_copy_samples, 99, 100)
         << ", \"operations\": " << copy_repetitions << "}\n"
         << "  ]\n"
         << "}\n";

    if (options->output.has_value()) {
        std::ofstream output(*options->output, std::ios::binary);
        if (!output) {
            std::cerr << "cannot open benchmark output\n";
            return 2;
        }
        output << json.str();
        if (!output) {
            std::cerr << "cannot write benchmark output\n";
            return 2;
        }
    } else {
        std::cout << json.str();
    }
    return 0;
}
