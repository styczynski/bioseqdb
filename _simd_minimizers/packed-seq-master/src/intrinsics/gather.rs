use wide::u64x4;

#[inline(always)]
#[cfg(target_feature = "avx2")]
unsafe fn gather_avx2(ptr: *const u8, offsets: u64x4) -> u64x4 {
    #[cfg(target_arch = "x86")]
    use core::arch::x86::_mm256_i64gather_epi64;
    #[cfg(target_arch = "x86_64")]
    use core::arch::x86_64::_mm256_i64gather_epi64;
    use core::mem::transmute;

    transmute(_mm256_i64gather_epi64::<1>(
        ptr as *const i64,
        transmute(offsets),
    ))
}

#[inline(always)]
unsafe fn gather_fallback(ptr: *const u8, offsets: u64x4) -> u64x4 {
    let source = u64x4::splat(ptr as u64) + offsets;
    u64x4::new(
        source
            .to_array()
            .map(|p| (p as *const u64).read_unaligned()),
    )
}

/// Given a base address and 4 offsets in bytes, read the 4 u64 values starting at the given positions.
#[inline(always)]
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub fn gather(ptr: *const u8, offsets: u64x4) -> u64x4 {
    #[cfg(target_feature = "avx2")]
    unsafe {
        gather_avx2(ptr, offsets)
    }
    #[cfg(not(target_feature = "avx2"))]
    unsafe {
        gather_fallback(ptr, offsets)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_gather() {
        let data = [0u8, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];
        let base = data.as_ptr();
        let offsets = u64x4::new([0, 3, 5, 8]);
        let values = gather(base, offsets);
        assert_eq!(
            values,
            u64x4::new([
                0x0706050403020100,
                0x0a09080706050403,
                0x0c0b0a0908070605,
                0x0f0e0d0c0b0a0908
            ])
        );
    }
}
