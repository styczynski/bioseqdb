/**
 * @file hashing.cppm
 * @brief Small exported 64-bit hashing helpers shared by logging and caches.
 */
module;
#include "common.h"

#include <cstdint>
#include <string_view>

export module bioseq.common.hashing;

export namespace bioseq::common {

[[nodiscard, gnu::always_inline]]
inline std::uint64_t fnv1a_64(std::string_view bytes) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

[[nodiscard, gnu::always_inline]]
inline std::uint64_t hash_combine_64(
    std::uint64_t seed,
    std::uint64_t value) noexcept
{
    return seed ^ (value + 0x9e3779b97f4a7c15ull +
                   (seed << 6) + (seed >> 2));
}

[[nodiscard, gnu::always_inline]]
inline std::uint64_t hash_combine_64(
    std::uint64_t seed,
    std::string_view value) noexcept
{
    return hash_combine_64(seed, fnv1a_64(value));
}

} // namespace bioseq::common
