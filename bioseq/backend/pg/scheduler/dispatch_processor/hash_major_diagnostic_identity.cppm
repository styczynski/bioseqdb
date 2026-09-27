/**
 * @file hash_major_diagnostic_identity.cppm
 * @brief Make compact diagnostic IDs for hash-major index sources.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>

export module bioseq.postgres.scheduler.dispatch_processor.hash_major_diagnostic_identity;

import bioseq.indexing.storage;

export namespace bioseq::simd {

/**
 * @brief Reduce a checked hash-major source identity to a diagnostic ID.
 * @param[in] source_identity The 128-bit identity supplied by the index reader.
 * @return A stable, nonzero token for diagnostic statistics.
 */
[[nodiscard]] constexpr std::uint64_t
hash_major_source_diagnostic_token(const std::array<std::byte, 16>& source_identity) noexcept {
    std::array<std::byte, 32> bytes{std::byte{'B'},
                                    std::byte{'S'},
                                    std::byte{'E'},
                                    std::byte{'Q'},
                                    std::byte{'F'},
                                    std::byte{'6'},
                                    std::byte{'D'},
                                    std::byte{'I'},
                                    std::byte{'A'},
                                    std::byte{'G'},
                                    std::byte{'v'},
                                    std::byte{'1'},
                                    std::byte{0},
                                    std::byte{0},
                                    std::byte{0},
                                    std::byte{0}};
    for (std::size_t index = 0; index < source_identity.size(); ++index) {
        bytes[16u + index] = source_identity[index];
    }
    const std::uint64_t token = bioseq::indexing::crc64_ecma182(bytes);
    return token == 0u ? 0x9e3779b97f4a7c15ull : token;
}

} // namespace bioseq::simd

namespace bioseq::simd {
namespace {
constexpr std::array<std::byte, 16> hash_major_diagnostic_identity_a{std::byte{1}};
constexpr std::array<std::byte, 16> hash_major_diagnostic_identity_b{std::byte{2}};
static_assert(hash_major_source_diagnostic_token(std::array<std::byte, 16>{}) != 0u);
static_assert(hash_major_source_diagnostic_token(hash_major_diagnostic_identity_a) != 0u);
static_assert(hash_major_source_diagnostic_token(hash_major_diagnostic_identity_a) ==
              hash_major_source_diagnostic_token(hash_major_diagnostic_identity_a));
static_assert(hash_major_source_diagnostic_token(hash_major_diagnostic_identity_a) !=
              hash_major_source_diagnostic_token(hash_major_diagnostic_identity_b));
} // namespace
} // namespace bioseq::simd
