//! Determine whether each window is canonical, when `#GT > #AC`.
use std::mem::transmute;

use crate::S;
use packed_seq::Delay;
use wide::{CmpGt, i32x8, u32x8};

/// An iterator over windows that returns for each whether it's canonical or not.
/// Canonical windows have >half TG characters.
/// Window length l=k+w-1 must be odd for this to never tie.
pub fn canonical_mapper_scalar(l: usize) -> (Delay, impl FnMut((u8, u8)) -> bool) {
    assert!(
        l % 2 == 1,
        "Window length l={l} must be odd to guarantee canonicality"
    );

    // Cnt of odd characters, offset by -l/2 so >0 is canonical and <0 is not.
    let mut cnt = -(l as isize);

    (
        Delay(l - 1),
        #[inline(always)]
        move |(a, r)| {
            cnt += (a & 2) as isize;
            let out = cnt > 0;
            cnt -= (r & 2) as isize;
            out
        },
    )
}

/// An iterator over windows that returns for each whether it's canonical or not.
/// Canonical windows have >half odd characters.
/// Window length l=k+w-1 must be odd for this to never tie.
///
/// Split the kmers of the sequence into 8 chunks of equal length ~len/8.
/// Then compute of each of them in parallel using SIMD,
/// and return the remaining few using the second iterator.
/// NOTE: First l-1 values are bogus.
#[inline(always)]
pub fn canonical_mapper_simd(l: usize) -> (Delay, impl FnMut((S, S)) -> u32x8) {
    assert!(
        l % 2 == 1,
        "Window length l={l} must be odd to guarantee canonicality"
    );

    // Cnt of odd characters, offset by -l/2 so >0 is canonical and <0 is not.
    let mut cnt = i32x8::splat(-(l as i32));
    let two = i32x8::splat(2);

    (
        Delay(l - 1),
        #[inline(always)]
        move |(a, r)| {
            cnt += unsafe { transmute::<_, i32x8>(a) } & two;
            let out = unsafe { transmute::<_, u32x8>(cnt.cmp_gt(i32x8::ZERO)) };
            cnt -= unsafe { transmute::<_, i32x8>(r) } & two;
            out
        },
    )
}
