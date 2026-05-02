/**
 * @file encoder_dispatch.cppm
 * @brief Compile-time dispatcher between the scalar and SIMD encoder
 *        paths.
 *
 * `encode_body_chunk` chooses between
 * `bioseq_io.encoder::encode_body_chunk_scalar` (any alphabet, scalar
 * 256-entry LUT) and the SIMD dna4 path imported from
 * `bioseq_io.simd_kernels` via the public `dna4_simd_encode_body_chunk`
 * non-inline wrapper. The `if constexpr` branches on the alphabet
 * template parameter.
 *
 * No `<hwy/highway.h>` here — the SIMD path is exposed as a
 * non-inline wrapper from `simd_kernels`, so this dispatcher stays
 * Highway-free at the source level.
 */
module;

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq_io.encoder_dispatch;

import sequan_compat;
import bioseq_sequences;
import bioseq_io.encoder;
import bioseq_io.simd_kernels;

export namespace bioseq::io {

/// Encode a contiguous span of body bytes into the per-record state.
/// Dispatches on `alphabet_t` at compile time: `seq::dna4` takes the
/// SIMD LUT path; everything else falls back to scalar.
///
/// `body_bytes` MUST NOT contain `\n` — the caller strips line
/// endings before passing the span. Spec §11.1 / §11.2.
template <sequence_alphabet alphabet_t,
          typename storage_unit_t,
          std::size_t BitsPerChar,
          std::size_t LettersPerUnit>
inline void encode_body_chunk(
    encode_state<storage_unit_t, BitsPerChar, LettersPerUnit>& state,
    const std::uint8_t* __restrict__ body_bytes,
    std::size_t body_size) {
    if constexpr (std::is_same_v<alphabet_t, seq::dna4>
               && std::is_same_v<storage_unit_t, std::uint64_t>
               && BitsPerChar == 2
               && LettersPerUnit == 32) {
        // Hot path: dna4 packed into uint64_t (32 letters / word).
        // The SIMD wrapper takes the encoder state by an opaque
        // adapter shape (see `simd_kernels.cppm` for the bridge).
        bioseq::io::simd::dna4_simd_encode_body_chunk(
            state, body_bytes, body_size);
    } else {
        encode_body_chunk_scalar<alphabet_t,
                                 storage_unit_t,
                                 BitsPerChar,
                                 LettersPerUnit>(
            state, body_bytes, body_size);
    }
}

}  // namespace bioseq::io
