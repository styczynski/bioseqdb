//! 'Anti lexicographic' hashing:
//!
//! A kmer's hash found by reading it's characters right to left, and by inverting the last (most significant) character.
//! When k > 16, only the last 16 characters are used.

use std::cmp::min;

use crate::{KmerHasher, S};
use packed_seq::{Delay, Seq};

/// A hash function that compares strings reverse-lexicographically,
/// with the last (most significant) character inverted.
///
/// Only supports 2-bit DNA sequences ([`packed_seq::AsciiSeq`] and [`packed_seq::PackedSeq`]).
///
/// The canonical version (with `CANONICAL=true`) returns the minimum of the forward and reverse-complement hashes.
/// TODO: Test minimum vs maximum.
pub struct AntiLexHasher<const CANONICAL: bool> {
    k: usize,
    /// Number of bits of each character.
    b: usize,
    /// Number of bits to shift each new character up to make it the most significant one.
    shift: u32,
    /// Mask to flip the bits of the most significant character.
    anti: u32,
    /// Mask to keep only the lowest k*b bits.
    mask: u32,
}

impl<const CANONICAL: bool> AntiLexHasher<CANONICAL> {
    /// Create a new [`AntiLexHasher`] for kmers of length `k`.
    #[inline(always)]
    pub const fn new(k: usize) -> Self {
        let b = 2;
        let shift = if b * k <= 32 { b * (k - 1) } else { 32 - b } as u32;
        let anti = ((1 << b) - 1) << shift;
        let mask = if b * k < 32 {
            (1 << (b * k)) - 1
        } else {
            u32::MAX
        };
        Self {
            k,
            b,
            shift,
            anti,
            mask,
        }
    }
}

impl KmerHasher for AntiLexHasher<false> {
    const CANONICAL: bool = false;

    fn new(k: usize) -> Self {
        Self::new(k)
    }

    #[inline(always)]
    fn k(&self) -> usize {
        self.k
    }

    #[inline(always)]
    fn in_out_mapper_scalar<'s>(&self, seq: impl Seq<'s>) -> impl FnMut((u8, u8)) -> u32 {
        assert!(seq.bits_per_char() <= self.b);

        let mut fw: u32 = 0;
        move |(a, _r)| {
            fw = (fw >> self.b) ^ ((a as u32) << self.shift);
            fw ^ self.anti
        }
    }

    #[inline(always)]
    fn in_out_mapper_simd<'s>(&self, seq: impl Seq<'s>) -> impl FnMut((S, S)) -> S {
        assert!(seq.bits_per_char() <= self.b);

        let mut fw: S = S::splat(0);
        move |(a, _r)| {
            fw = (fw >> self.b as u32) ^ (a << self.shift);
            fw ^ S::splat(self.anti)
        }
    }

    #[inline(always)]
    fn mapper<'s>(&self, seq: impl Seq<'s>) -> impl FnMut(u8) -> u32 {
        assert!(seq.bits_per_char() <= self.b);
        let k = seq.len();
        let shift = if self.b * k <= 32 {
            self.b * (k - 1)
        } else {
            32 - self.b
        } as u32;
        let anti = ((1 << self.b) - 1) << shift;

        let mut fw: u32 = 0;
        move |a| {
            fw = (fw >> self.b) ^ ((a as u32) << shift);
            fw ^ anti
        }
    }
}

impl KmerHasher for AntiLexHasher<true> {
    const CANONICAL: bool = true;

    fn new(k: usize) -> Self {
        Self::new(k)
    }

    #[inline(always)]
    fn k(&self) -> usize {
        self.k
    }

    #[inline(always)]
    fn delay(&self) -> Delay {
        Delay(self.k.saturating_sub(32 / self.b))
    }

    #[inline(always)]
    fn mapper<'s>(&self, seq: impl Seq<'s>) -> impl FnMut(u8) -> u32 {
        assert!(seq.bits_per_char() <= self.b);
        let mut shift = 0;
        let mut anti = (1 << self.b) - 1;
        let mut mask = anti;

        let mut fw: u32 = 0;
        let mut rc: u32 = 0;
        let mut i = 0;
        move |a| {
            if i * self.b >= 32 {
                fw >>= self.b;
            }
            fw ^= (a as u32) << shift;
            if i * self.b < 32 {
                // ^2 for complement.
                rc = ((rc << self.b) & mask) ^ (a as u32 ^ 2);
            }
            let out = min(fw ^ anti, rc ^ anti);

            if (i + 1) * self.b < 32 {
                shift += self.b as u32;
                anti <<= self.b;
                mask = (mask << self.b) | ((1 << self.b) - 1);
            }
            i += 1;

            out
        }
    }

    #[inline(always)]
    fn in_out_mapper_scalar<'s>(&self, seq: impl Seq<'s>) -> impl FnMut((u8, u8)) -> u32 {
        assert!(seq.bits_per_char() <= self.b);

        let mut fw: u32 = 0;
        let mut rc: u32 = 0;
        move |(a, r)| {
            fw = (fw >> self.b) ^ ((a as u32) << self.shift);
            // ^2 for complement.
            rc = ((rc << self.b) & self.mask) ^ (r as u32 ^ 2);
            min(fw ^ self.anti, rc ^ self.anti)
        }
    }

    #[inline(always)]
    fn in_out_mapper_simd<'s>(&self, seq: impl Seq<'s>) -> impl FnMut((S, S)) -> S {
        assert!(seq.bits_per_char() <= self.b);

        let mut fw: S = S::splat(0);
        let mut rc: S = S::splat(0);
        move |(a, r)| {
            fw = (fw >> self.b as u32) ^ (a << self.shift);
            rc = ((rc << self.b as u32) & S::splat(self.mask)) ^ (r ^ S::splat(2));
            (fw ^ S::splat(self.anti)).min(rc ^ S::splat(self.anti))
        }
    }
}
