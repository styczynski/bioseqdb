//////////////////////////// Bit-packed SIMD ////////////////////////////

use super::*;
use std::{
    array::from_fn,
    mem::transmute,
    simd::{Simd, ptr::SimdConstPtr},
};

const L: usize = 8;
type T = u32;
type S = Simd<T, L>;

pub struct FxHashSimd;

impl ParHasher<L> for FxHashSimd {
    type Out = T;
    #[inline(always)]
    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = [Self::Out; L]> {
        FxHashPackedSimdIt::new(t, k).unwrap().map(|x| x.into())
    }
}

#[derive(Debug)]
pub struct FxHashPackedSimdIt<'a> {
    _seq: &'a [u8],
    n: usize,
    k: usize,
    chars: S,
    mask: S,
    constant: S,
    current_idx: usize,
    offsets: Simd<*const u8, 4>,
    offsets_next: Simd<*const u8, 4>,
    upcoming_chars: S,
    upcoming_chars_next: S,
}

impl<'a> FxHashPackedSimdIt<'a> {
    /// Creates a new FxHashIt with internal state properly initialized.
    #[inline(always)]
    pub fn new(seq: &'a [u8], k: usize) -> Option<Self> {
        assert!(k <= 16);
        if k > seq.len() {
            return None;
        }
        if k > MAXIMUM_K_SIZE {
            return None;
        }
        if seq.len() > u32::MAX as _ {
            return None;
        }
        let num_kmers = 4 * seq.len() - k + 1;
        // if num_kmers % L != 0 {
        //     return None;
        // }
        let n = num_kmers / L;

        Some(Self {
            _seq: seq,
            n,
            k,
            chars: S::splat(0),
            current_idx: 0,
            mask: S::splat((1 << (2 * k)) - 1),
            constant: S::splat(1234565323),
            offsets: from_fn(|l| unsafe { seq.as_ptr().add(l * n) }).into(),
            offsets_next: from_fn(|l| unsafe { seq.as_ptr().add((4 + l) * n) }).into(),
            upcoming_chars: S::splat(0),
            upcoming_chars_next: S::splat(0),
        })
    }
}

impl<'a> Iterator for FxHashPackedSimdIt<'a> {
    type Item = S;

    #[inline(always)]
    fn next(&mut self) -> Option<S> {
        if self.current_idx == self.n {
            return None;
        };

        let i = self.current_idx;
        self.current_idx += 1;
        let read = |i| unsafe {
            let oi1 = self.offsets.wrapping_add(Simd::splat(i / 4));
            let oi2 = self.offsets_next.wrapping_add(Simd::splat(i / 4));
            let chars_i1: Simd<u32, 8> = transmute(Simd::<u64, 4>::gather_ptr(transmute(oi1)));
            let chars_i2: Simd<u32, 8> = transmute(Simd::<u64, 4>::gather_ptr(transmute(oi2)));
            chars_i1.deinterleave(chars_i2)
        };
        if (i + self.k) % 16 == 0 {
            if (i + self.k) % 32 == 0 {
                (self.upcoming_chars, self.upcoming_chars_next) = read(i + self.k);
            } else {
                self.upcoming_chars = self.upcoming_chars_next;
            }
        }
        // Extract the last 2 bits of each character.
        let new_chars = self.upcoming_chars & S::splat(0x03);
        // Shift remaining characters to the right.
        self.upcoming_chars >>= S::splat(2);
        // Rotate the bit-representation.
        self.chars <<= S::splat(2);
        // Mask the previous
        self.chars &= self.mask;
        // Or in the new bits.
        self.chars |= new_chars;
        // Multiply by a constant.
        let hashes = self.chars * self.constant;
        Some(hashes)
    }
}
