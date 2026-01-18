use crate::S;

/// Given a 'table' `t` consisting of 8 values, and an index `idx` consisting of 8 indices from 0 to 4,
/// look up the first four indices in the first half of `t`, and the second four indices in the second half of `t`.
#[inline(always)]
pub fn table_lookup(t: S, idx: S) -> S {
    _table_lookup(t, idx)
}

#[inline(always)]
#[cfg(target_feature = "avx")]
fn _table_lookup(t: S, idx: S) -> S {
    unsafe {
        #[cfg(target_arch = "x86")]
        use core::arch::x86::_mm256_permutevar_ps;
        #[cfg(target_arch = "x86_64")]
        use core::arch::x86_64::_mm256_permutevar_ps;
        use core::mem::transmute;

        transmute(_mm256_permutevar_ps(transmute(t), transmute(idx)))
    }
}

#[inline(always)]
#[cfg(target_feature = "neon")]
fn _table_lookup(t: S, idx: S) -> S {
    unsafe {
        use core::arch::aarch64::{uint8x16_t, vqtbl1q_u8};
        use core::mem::transmute;

        const OFFSET: S = unsafe { std::mem::transmute([0x03_02_01_00; 8]) };
        const MASK: S = unsafe { std::mem::transmute([0x04_04_04_04; 8]) };

        let idx = idx * MASK + OFFSET;
        let (t1, t2): (uint8x16_t, uint8x16_t) = transmute(t);
        let (i1, i2): (uint8x16_t, uint8x16_t) = transmute(idx);
        let r1 = vqtbl1q_u8(t1, i1);
        let r2 = vqtbl1q_u8(t2, i2);
        transmute((r1, r2))
    }
}

#[inline(always)]
#[cfg(not(any(target_feature = "avx", target_feature = "neon")))]
fn _table_lookup(t: S, idx: S) -> S {
    unsafe {
        let t = t.as_array_ref();
        S::new(idx.to_array().map(|i| *t.get_unchecked(i as usize)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_table_lookup() {
        let t = S::new([1000, 1001, 1002, 1003, 1000, 1001, 1002, 1003]);
        let idx = S::new([2, 0, 3, 1, 0, 2, 1, 0]);
        let res = table_lookup(t, idx);

        assert_eq!(
            res.to_array(),
            [1002, 1000, 1003, 1001, 1000, 1002, 1001, 1000]
        );
    }
}
