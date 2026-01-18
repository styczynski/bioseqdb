use wide::u32x8;

#[inline(always)]
#[cfg(target_feature = "avx2")]
unsafe fn deinterleave_avx(a: u32x8, b: u32x8) -> (u32x8, u32x8) {
    #[cfg(target_arch = "x86")]
    use core::arch::x86::{__m256, __m256d, _mm256_permute4x64_pd, _mm256_shuffle_ps};
    #[cfg(target_arch = "x86_64")]
    use core::arch::x86_64::{__m256, __m256d, _mm256_permute4x64_pd, _mm256_shuffle_ps};
    use core::mem::transmute;

    const SHUFFLE_EVEN: i32 = 0b10_00_10_00;
    const SHUFFLE_ODD: i32 = 0b11_01_11_01;
    const SWAP_MIDDLE: i32 = 0b11_01_10_00;

    let a: __m256 = transmute(a);
    let b: __m256 = transmute(b);
    let abab_even: __m256d = transmute(_mm256_shuffle_ps(a, b, SHUFFLE_EVEN));
    let abab_odd: __m256d = transmute(_mm256_shuffle_ps(a, b, SHUFFLE_ODD));
    let ab_even = _mm256_permute4x64_pd(abab_even, SWAP_MIDDLE);
    let ab_odd = _mm256_permute4x64_pd(abab_odd, SWAP_MIDDLE);
    (transmute(ab_even), transmute(ab_odd))
}

#[inline(always)]
#[cfg(target_feature = "neon")]
unsafe fn deinterleave_neon(a: u32x8, b: u32x8) -> (u32x8, u32x8) {
    use core::arch::aarch64::{uint32x4_t, vuzp1q_u32, vuzp2q_u32};
    use core::mem::transmute;

    let (a1, a2): (uint32x4_t, uint32x4_t) = transmute(a);
    let (b1, b2): (uint32x4_t, uint32x4_t) = transmute(b);
    let a_even = vuzp1q_u32(a1, a2);
    let a_odd = vuzp2q_u32(a1, a2);
    let b_even = vuzp1q_u32(b1, b2);
    let b_odd = vuzp2q_u32(b1, b2);
    let ab_even: u32x8 = transmute((a_even, b_even));
    let ab_odd: u32x8 = transmute((a_odd, b_odd));
    (ab_even, ab_odd)
}

#[inline(always)]
unsafe fn deinterleave_fallback(a: u32x8, b: u32x8) -> (u32x8, u32x8) {
    let a = a.as_array_ref();
    let b = b.as_array_ref();
    (
        u32x8::new([
            *a.get_unchecked(0),
            *a.get_unchecked(2),
            *a.get_unchecked(4),
            *a.get_unchecked(6),
            *b.get_unchecked(0),
            *b.get_unchecked(2),
            *b.get_unchecked(4),
            *b.get_unchecked(6),
        ]),
        u32x8::new([
            *a.get_unchecked(1),
            *a.get_unchecked(3),
            *a.get_unchecked(5),
            *a.get_unchecked(7),
            *b.get_unchecked(1),
            *b.get_unchecked(3),
            *b.get_unchecked(5),
            *b.get_unchecked(7),
        ]),
    )
}

/// Given two SIMD vectors `a` and `b`, return one vector with elements at even
/// positions in `a` and `b` and one vector with elements at odd positions.
#[inline(always)]
pub fn deinterleave(a: u32x8, b: u32x8) -> (u32x8, u32x8) {
    #[cfg(target_feature = "avx2")]
    unsafe {
        deinterleave_avx(a, b)
    }
    #[cfg(target_feature = "neon")]
    unsafe {
        deinterleave_neon(a, b)
    }
    #[cfg(not(any(target_feature = "avx2", target_feature = "neon")))]
    unsafe {
        deinterleave_fallback(a, b)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_deinterleave() {
        let a = u32x8::new([0, 1, 2, 3, 4, 5, 6, 7]);
        let b = u32x8::new([8, 9, 10, 11, 12, 13, 14, 15]);
        let (c, d) = deinterleave(a, b);

        assert_eq!(c.to_array(), [0, 2, 4, 6, 8, 10, 12, 14]);
        assert_eq!(d.to_array(), [1, 3, 5, 7, 9, 11, 13, 15]);
    }
}
