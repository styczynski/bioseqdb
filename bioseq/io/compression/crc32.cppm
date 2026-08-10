/**
 * @file crc32.cppm
 * @brief CRC-32/ISO-HDLC (gzip) wrapper + Highway entry-point declaration.
 *
 * Forwards `crc32_update` to zlib's `crc32_z` (hardware-accelerated on
 * Apple Silicon via PMULL and on x86 via PCLMULQDQ). The Highway path
 * `crc32_iso_hdlc_simd` is exposed but not wired in this PR — the
 * orchestrator keeps calling `crc32_update`. A separate A/B PR flips
 * the switch.
 *
 * Coefficients (`iso_hdlc_reflected_keys`) and constants (`iso_hdlc_initial_value`,
 * `iso_hdlc_final_xor`) are sourced from
 * `crc-fast-rust-main/src/crc32/consts.rs:489-513`.
 */
module;


#include <zlib.h>

#include <array>
#include <cstddef>
#include <cstdint>

export module bioseq.io.compression.crc32;


export namespace bioseq::io::compression {

/// CRC-32/ISO-HDLC (gzip) fold coefficients — reflected mode. Values
/// from `crc-fast-rust-main/src/crc32/consts.rs:489-513`. Index 0 is
/// unused (1-based to match the Rust port's comments). Indices 7-8 are
/// Barrett's `mu` and `P(x)`.
inline constexpr std::array<std::uint64_t, 23> iso_hdlc_reflected_keys = {
    0x0000000000000000ULL,
    0x00000000ccaa009eULL, 0x00000001751997d0ULL, 0x000000014a7fe880ULL,
    0x00000001e88ef372ULL, 0x00000000ccaa009eULL, 0x0000000163cd6124ULL,
    0x00000001f7011641ULL, 0x00000001db710641ULL, 0x00000001d7cfc6acULL,
    0x00000001ea89367eULL, 0x000000018cb44e58ULL, 0x00000000df068dc2ULL,
    0x00000000ae0b5394ULL, 0x00000001c7569e54ULL, 0x00000001c6e41596ULL,
    0x0000000154442bd4ULL, 0x0000000174359406ULL, 0x000000003db1ecdcULL,
    0x000000015a546366ULL, 0x00000000f1da05aaULL, 0x00000001322d1430ULL,
    0x000000011542778aULL,
};

/// Initial state for a fresh gzip member: 0xFFFFFFFF (RFC 1952).
inline constexpr std::uint32_t iso_hdlc_initial_value  = 0xFFFFFFFFu;
/// Output xor: 0xFFFFFFFF (RFC 1952).
inline constexpr std::uint32_t iso_hdlc_final_xor = 0xFFFFFFFFu;

/// Cast helpers — bridge bioseq's `std::uint*_t` / `std::size_t` types
/// to zlib's `uLong` / `z_size_t` / `z_off_t` without scattering casts
/// at each call site.
namespace detail {

[[nodiscard]] inline uLong to_zlib_ulong(std::uint32_t value) noexcept {
    return static_cast<uLong>(value);
}
[[nodiscard]] inline const Bytef* to_zlib_bytes(
    const std::uint8_t* pointer) noexcept {
    return reinterpret_cast<const Bytef*>(pointer);
}
[[nodiscard]] inline z_size_t to_zlib_size(std::size_t value) noexcept {
    return static_cast<z_size_t>(value);
}
[[nodiscard]] inline z_off_t to_zlib_off(std::size_t value) noexcept {
    return static_cast<z_off_t>(value);
}
[[nodiscard]] inline std::uint32_t from_zlib_ulong(uLong value) noexcept {
    return static_cast<std::uint32_t>(value);
}

}  // namespace detail

/// Update a running CRC32 with `bytes`. Forwards to zlib's `crc32_z`.
[[nodiscard]] inline std::uint32_t crc32_update(
    std::uint32_t        running,
    const std::uint8_t*  bytes,
    std::size_t          size) noexcept {
    return detail::from_zlib_ulong(::crc32_z(
        detail::to_zlib_ulong(running),
        detail::to_zlib_bytes(bytes),
        detail::to_zlib_size(size)));
}

[[nodiscard]] inline std::uint32_t crc32_combine(
    std::uint32_t crc_a, std::uint32_t crc_b, std::size_t length_b) noexcept {
    return detail::from_zlib_ulong(::crc32_combine(
        detail::to_zlib_ulong(crc_a),
        detail::to_zlib_ulong(crc_b),
        detail::to_zlib_off(length_b)));
}

/// Highway-acceleration entry point. Implementation lives in
/// `simd.cppm`. **Not wired** by default: the orchestrator
/// continues to call `crc32_update` (which routes to zlib's crc32_z).
[[nodiscard]] std::uint32_t crc32_iso_hdlc_simd(
    std::uint32_t running, const std::uint8_t* bytes, std::size_t size) noexcept;

}  // namespace bioseq::io::compression
