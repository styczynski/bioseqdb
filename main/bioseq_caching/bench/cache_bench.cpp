#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

std::uint64_t median(std::vector<std::uint64_t> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2u];
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

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else if (argument.starts_with("--output=")) {
            options.output = std::string(argument.substr(9));
        } else if (argument.starts_with("--rows=")) {
            options.rows = std::stoull(std::string(argument.substr(7)));
        } else if (argument.starts_with("--samples=")) {
            options.samples = static_cast<std::uint32_t>(
                std::stoul(std::string(argument.substr(10))));
        } else if (!argument.starts_with('-') && !options.output.has_value()) {
            options.output = std::string(argument);
        } else {
            return std::nullopt;
        }
    }
    if (options.rows == 0 || options.samples == 0) {
        return std::nullopt;
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

    std::vector<std::uint64_t> decode_samples;
    decode_samples.reserve(options->samples);
    std::uint64_t decoded_accumulator = 0;
    for (std::uint32_t sample = 0; sample < options->samples; ++sample) {
        const auto started = Clock::now();
        auto view = validate_column_store(blob, kIdentity, checksums);
        if (!view.has_value()) {
            std::cerr << "column validation failed\n";
            return 1;
        }
        std::uint64_t local_accumulator = 0;
        for (std::uint64_t row = 0; row < view->row_count(); ++row) {
            const auto score = view->value(row, 1);
            const auto payload = view->value(row, 2);
            if (!score.has_value() || !payload.has_value()
                || score->bytes.size() != 4 || payload->bytes.size() != 16) {
                std::cerr << "column replay failed\n";
                return 1;
            }
            local_accumulator += std::to_integer<std::uint8_t>(score->bytes[0]);
            local_accumulator += std::to_integer<std::uint8_t>(payload->bytes[0]);
        }
        const auto finished = Clock::now();
        decoded_accumulator ^= local_accumulator;
        decode_samples.push_back(static_cast<std::uint64_t>(
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
    std::ostringstream json;
    json << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"rows\": " << options->rows << ",\n"
         << "  \"samples\": " << options->samples << ",\n"
         << "  \"correctness\": {\n"
         << "    \"column_blob_digest\": \"" << hex_digest(*digest)
         << "\",\n"
         << "    \"decoded_accumulator\": " << decoded_accumulator << ",\n"
         << "    \"admission_final_reserved_bytes\": 0\n"
         << "  },\n"
         << "  \"benchmarks\": [\n"
         << "    {\"name\": \"column_encode\", \"p50_ns\": "
         << median(encode_samples) << ", \"operations\": "
         << options->rows << "},\n"
         << "    {\"name\": \"column_validate_decode\", \"p50_ns\": "
         << median(decode_samples) << ", \"operations\": "
         << options->rows << "},\n"
         << "    {\"name\": \"admission_reserve_release\", \"p50_ns\": "
         << median(admission_samples) << ", \"operations\": "
         << admission_operations << "}\n"
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
