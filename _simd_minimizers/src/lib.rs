//! A library to quickly compute (canonical) minimizers of DNA and text sequences.
//!
//! The main functions are:
//! - [`minimizer_positions`]: compute the positions of all minimizers of a sequence.
//! - [`canonical_minimizer_positions`]: compute the positions of all _canonical_ minimizers of a sequence.
//!
//! Adjacent equal positions are deduplicated, but since the canonical minimizer is _not_ _forward_, a position could appear more than once.
//!
//! The implementation uses SIMD by splitting each sequence into 8 chunks and processing those in parallel.
//!
//! When using super-k-mers, use the `_and_superkmer` variants to additionally return a vector containing the index of the first window the minimizer is minimal.
//!
//! The minimizer of a single window can be found using [`one_minimizer`] and [`one_canonical_minimizer`], but note that these functions are not nearly as efficient.
//!
//! The `scalar` versions are mostly for testing only, and basically always slower.
//!
//! ## Minimizers
//!
//! The code is explained in detail in our [paper](https://doi.org/10.4230/LIPIcs.SEA.2025.20):
//!
//! > SimdMinimizers: Computing random minimizers, fast.
//! > Ragnar Groot Koerkamp, Igor Martayan, SEA 2025
//!
//! Briefly, minimizers are defined using two parameters `k` and `w`.
//! Given a sequence of characters, all k-mers (substrings of length `k`) are hashed,
//! and for each _window_ of `k` consecutive k-mers (of length `l = w + k - 1` characters),
//! (the position of) the smallest k-mer is sampled.
//!
//! Minimizers are found as follows:
//! 1. Split the input to 8 chunks that are processed in parallel using SIMD.
//! 2. Compute a 32-bit ntHash rolling hash of the k-mers.
//! 3. Use the 'two stacks' sliding window minimum on the top 16 bits of each hash.
//! 4. Break ties towards the leftmost position by storing the position in the bottom 16 bits.
//! 5. Compute 8 consecutive minimizer positions, and dedup them.
//! 6. Collect the deduplicated minimizer positions from all 8 chunks into a single vector.
//!
//! ## Canonical minimizers
//!
//! _Canonical_ minimizers have the property that the sampled k-mers of a DNA sequence are the same as those sampled from the _reverse complement_ sequence.
//!
//! This works as follows:
//! 1. ntHash is modified to use the canonical version that computes the xor of the hash of the forward and reverse complement k-mer.
//! 2. Compute the leftmost and rightmost minimal k-mer.
//! 3. Compute the 'preferred' strand of the current window as the one with more `TG` characters. This requires `l=w+k-1` to be odd for proper tie-breaking.
//! 4. Return either the leftmost or rightmost smallest k-mer, depending on the preferred strand.
//!
//! ## Syncmers
//!
//! _Syncmers_ are (in our notation) windows of length `l = w + k - 1` characters where the minimizer k-mer is a prefix or suffix.
//! (Or, in classical notation, `k`-mers with the smallest `s`-mer as prefix or suffix.)
//! These can be computed by using [`fn@syncmers`] or [`canonical_syncmers`] instead of [`minimizers`] or [`canonical_minimizers`].
//!
//! Note that canonical syncmers are chosen as the minimum of the forward and reverse-complement k-mer representation.
//!
//! ## Input types
//!
//! This crate depends on [`packed_seq`] to handle generic types of input sequences.
//! Most commonly, one should use [`packed_seq::PackedSeqVec`] for packed DNA sequences, but one can also simply wrap a sequence of `ACTGactg` characters in [`packed_seq::AsciiSeqVec`].
//! Additionally, `simd_minimizers` works on general (ASCII) `&[u8]` text.
//!
//! The main function provided by [`packed_seq`] is [`packed_seq::Seq::iter_bp`], which splits the input into 8 chunks and iterates them in parallel using SIMD.
//!
//! When dealing with ASCII input, use the `AsciiSeq` and `AsciiSeqVec` types.
//!
//! ## Hash function
//!
//! By default, the library uses the `ntHash` hash function, which maps each DNA base `ACTG` to a pseudo-random value using a table lookup.
//! This hash function is specifically designed to be fast for hashing DNA sequences with input type [`packed_seq::PackedSeq`] and [`packed_seq::AsciiSeq`].
//!
//! For general ASCII sequences (`&[u8]`), `mulHash` is used instead, which instead multiplies each character value by a pseudo-random constant.
//! The `mul_hash` module provides functions that _always_ use mulHash, also for DNA sequences.
//!
//! ## Performance
//!
//! This library depends on AVX2 or NEON SIMD instructions to achieve good performance.
//! Make sure to compile with `-C target-cpu=native` to enable these instructions.
//! See the [ensure_simd](https://github.com/ragnargrootkoerkamp/ensure_simd) crate for more details.
//!
//! All functions take a `out_vec: &mut Vec<u32>` parameter to which positions are _appended_.
//! For best performance, re-use the same `out_vec` between invocations, and [`Vec::clear`] it before or after each call.
//!
//! ## Examples
//!
//! #### Scalar `AsciiSeq`
//!
//! ```
//! // Scalar ASCII version.
//! use packed_seq::{SeqVec, AsciiSeq};
//!
//! let seq = b"ACGTGCTCAGAGACTCAG";
//! let ascii_seq = AsciiSeq(seq);
//!
//! let k = 5;
//! let w = 7;
//!
//! let positions = simd_minimizers::minimizer_positions(ascii_seq, k, w);
//! assert_eq!(positions, vec![4, 5, 8, 13]);
//! ```
//!
//! #### SIMD `PackedSeq`
//!
//! ```
//! // Packed SIMD version.
//! use packed_seq::{PackedSeqVec, SeqVec, Seq};
//!
//! let seq = b"ACGTGCTCAGAGACTCAGAGGA";
//! let packed_seq = PackedSeqVec::from_ascii(seq);
//!
//! let k = 5;
//! let w = 7;
//!
//! // Unfortunately, `PackedSeqVec` can not `Deref` into a `PackedSeq`, so `as_slice` is needed.
//! // Since we also need the values, this uses the Builder API.
//! let mut fwd_pos = vec![];
//! let fwd_vals: Vec<_> = simd_minimizers::canonical_minimizers(k, w).run(packed_seq.as_slice(), &mut fwd_pos).values_u64().collect();
//! assert_eq!(fwd_pos, vec![0, 7, 9, 15]);
//! assert_eq!(fwd_vals, vec![
//!     // T  G  C  A  C, CACGT is rc of ACGTG at pos 0
//!     0b10_11_01_00_01,
//!     // G  A  G  A  C, CAGAG is at pos 7
//!     0b11_00_11_00_01,
//!     // C  A  G  A  G, GAGAC is at pos 9
//!     0b01_00_11_00_11,
//!     // G  A  G  A  C, CAGAG is at pos 15
//!     0b11_00_11_00_01
//! ]);
//!
//! // Check that reverse complement sequence has minimizers at 'reverse' positions.
//! let rc_packed_seq = packed_seq.as_slice().to_revcomp();
//! let mut rc_pos = Vec::new();
//! let mut rc_vals: Vec<_> = simd_minimizers::canonical_minimizers(k, w).run(rc_packed_seq.as_slice(), &mut rc_pos).values_u64().collect();
//! assert_eq!(rc_pos, vec![2, 8, 10, 17]);
//! for (fwd, &rc) in std::iter::zip(fwd_pos, rc_pos.iter().rev()) {
//!     assert_eq!(fwd as usize, seq.len() - k - rc as usize);
//! }
//! rc_vals.reverse();
//! assert_eq!(rc_vals, fwd_vals);
//! ```
//!
//! #### Seeded hasher
//!
//! ```
//! // Packed SIMD version with seeded hashes.
//! use packed_seq::{PackedSeqVec, SeqVec};
//!
//! let seq = b"ACGTGCTCAGAGACTCAG";
//! let packed_seq = PackedSeqVec::from_ascii(seq);
//!
//! let k = 5;
//! let w = 7;
//! let seed = 101010;
//! // Canonical by default. Use `NtHasher<false>` for forward-only.
//! let hasher = <seq_hash::NtHasher>::new_with_seed(k, seed);
//!
//! let fwd_pos = simd_minimizers::canonical_minimizers(k, w).hasher(&hasher).run_once(packed_seq.as_slice());
//! ```

#![allow(clippy::missing_transmute_annotations)]

mod canonical;
pub mod collect;
mod minimizers;
mod sliding_min;
pub mod syncmers;
mod intrinsics {
    mod dedup;
    pub use dedup::{append_filtered_vals, append_unique_vals, append_unique_vals_2};
}

#[cfg(test)]
mod test;

/// Re-exported internals. Used for benchmarking, and not part of the semver-compatible stable API.
pub mod private {
    pub mod canonical {
        pub use crate::canonical::*;
    }
    pub mod minimizers {
        pub use crate::minimizers::*;
    }
    pub mod sliding_min {
        pub use crate::sliding_min::*;
    }
    pub use packed_seq::u32x8 as S;
}

use collect::CollectAndDedup;
use collect::collect_and_dedup_into_scalar;
use collect::collect_and_dedup_with_index_into_scalar;
use minimizers::canonical_minimizers_skip_ambiguous_windows;
/// Re-export of the `packed-seq` crate.
pub use packed_seq;
use packed_seq::PackedNSeq;
use packed_seq::PackedSeq;
/// Re-export of the `seq-hash` crate.
pub use seq_hash;

use minimizers::{
    canonical_minimizers_seq_scalar, canonical_minimizers_seq_simd, minimizers_seq_scalar,
    minimizers_seq_simd,
};
use packed_seq::Seq;
use packed_seq::u32x8 as S;
use seq_hash::KmerHasher;

pub use minimizers::one_minimizer;
use seq_hash::NtHasher;
pub use sliding_min::Cache;
use syncmers::CollectSyncmers;
use syncmers::collect_syncmers_scalar;

thread_local! {
    static CACHE: std::cell::RefCell<(Cache, Vec<S>, Vec<S>)> = std::cell::RefCell::new(Default::default());
}

/// `CANONICAL`: true for canonical minimizers.
/// `H`: the kmer hasher to use.
/// `SkPos`: type of super-k-mer position storage. Use `()` to disable super-k-mers.
/// `SYNCMER`: 0 for minimizers, 1 for closed syncmers, 2 for open syncmers.
pub struct Builder<'h, const CANONICAL: bool, H: KmerHasher, SkPos, const SYNCMER: u8> {
    k: usize,
    w: usize,
    hasher: Option<&'h H>,
    sk_pos: SkPos,
}

pub struct Output<'o, const CANONICAL: bool, S> {
    /// k for minimizers, k+w-1 for syncmers
    len: usize,
    seq: S,
    min_pos: &'o Vec<u32>,
}

#[must_use]
pub const fn minimizers(k: usize, w: usize) -> Builder<'static, false, NtHasher<false>, (), 0> {
    Builder {
        k,
        w,
        hasher: None,
        sk_pos: (),
    }
}

#[must_use]
pub const fn canonical_minimizers(
    k: usize,
    w: usize,
) -> Builder<'static, true, NtHasher<true>, (), 0> {
    Builder {
        k,
        w,
        hasher: None,
        sk_pos: (),
    }
}

/// Return positions/values of *closed* syncmers of length `k+w-1`.
///
/// These are windows with the minimizer at the start or end of the window.
///
/// `k` here corresponds to `s` in original syncmer notation: the minimizer length.
/// `k+w-1` corresponds to `k` in original syncmer notation: the length of the extracted string.
#[must_use]
pub const fn closed_syncmers(
    k: usize,
    w: usize,
) -> Builder<'static, false, NtHasher<false>, (), 1> {
    Builder {
        k,
        w,
        hasher: None,
        sk_pos: (),
    }
}

#[must_use]
pub const fn canonical_closed_syncmers(
    k: usize,
    w: usize,
) -> Builder<'static, true, NtHasher<true>, (), 1> {
    Builder {
        k,
        w,
        hasher: None,
        sk_pos: (),
    }
}

/// Return positions/values of *open* syncmers of length `k+w-1`.
///
/// These are windows with the minimizer in the middle of the window. This requires `w` to be odd.
///
/// `k` here corresponds to `s` in original syncmer notation: the minimizer length.
/// `k+w-1` corresponds to `k` in original syncmer notation: the length of the extracted string.
#[must_use]
pub const fn open_syncmers(k: usize, w: usize) -> Builder<'static, false, NtHasher<false>, (), 2> {
    Builder {
        k,
        w,
        hasher: None,
        sk_pos: (),
    }
}

#[must_use]
pub const fn canonical_open_syncmers(
    k: usize,
    w: usize,
) -> Builder<'static, true, NtHasher<true>, (), 2> {
    Builder {
        k,
        w,
        hasher: None,
        sk_pos: (),
    }
}

impl<const CANONICAL: bool, const SYNCMERS: u8>
    Builder<'static, CANONICAL, NtHasher<CANONICAL>, (), SYNCMERS>
{
    #[must_use]
    pub const fn hasher<'h, H2: KmerHasher>(
        &self,
        hasher: &'h H2,
    ) -> Builder<'h, CANONICAL, H2, (), SYNCMERS> {
        Builder {
            k: self.k,
            w: self.w,
            sk_pos: (),
            hasher: Some(hasher),
        }
    }
}
impl<'h, const CANONICAL: bool, H: KmerHasher> Builder<'h, CANONICAL, H, (), 0> {
    #[must_use]
    pub const fn super_kmers<'o2>(
        &self,
        sk_pos: &'o2 mut Vec<u32>,
    ) -> Builder<'h, CANONICAL, H, &'o2 mut Vec<u32>, 0> {
        Builder {
            k: self.k,
            w: self.w,
            hasher: self.hasher,
            sk_pos,
        }
    }
}

/// Without-superkmer version
impl<'h, const CANONICAL: bool, H: KmerHasher, const SYNCMERS: u8>
    Builder<'h, CANONICAL, H, (), SYNCMERS>
{
    pub fn run_scalar_once<'s, SEQ: Seq<'s>>(&self, seq: SEQ) -> Vec<u32> {
        let mut min_pos = vec![];
        self.run_impl::<false, _>(seq, &mut min_pos);
        min_pos
    }

    pub fn run_once<'s, SEQ: Seq<'s>>(&self, seq: SEQ) -> Vec<u32> {
        let mut min_pos = vec![];
        self.run_impl::<true, _>(seq, &mut min_pos);
        min_pos
    }

    pub fn run_scalar<'s, 'o, SEQ: Seq<'s>>(
        &self,
        seq: SEQ,
        min_pos: &'o mut Vec<u32>,
    ) -> Output<'o, CANONICAL, SEQ> {
        self.run_impl::<false, _>(seq, min_pos)
    }

    pub fn run<'s, 'o, SEQ: Seq<'s>>(
        &self,
        seq: SEQ,
        min_pos: &'o mut Vec<u32>,
    ) -> Output<'o, CANONICAL, SEQ> {
        self.run_impl::<true, _>(seq, min_pos)
    }

    fn run_impl<'s, 'o, const SIMD: bool, SEQ: Seq<'s>>(
        &self,
        seq: SEQ,
        min_pos: &'o mut Vec<u32>,
    ) -> Output<'o, CANONICAL, SEQ> {
        let default_hasher = self.hasher.is_none().then(|| H::new(self.k));
        let hasher = self
            .hasher
            .unwrap_or_else(|| default_hasher.as_ref().unwrap());

        CACHE.with_borrow_mut(|cache| match (SIMD, CANONICAL, SYNCMERS) {
            (false, false, 0) => collect_and_dedup_into_scalar(
                minimizers_seq_scalar(seq, hasher, self.w, &mut cache.0),
                min_pos,
            ),
            (false, false, 1) => collect_syncmers_scalar::<false>(
                self.w,
                minimizers_seq_scalar(seq, hasher, self.w, &mut cache.0),
                min_pos,
            ),
            (false, false, 2) => collect_syncmers_scalar::<true>(
                self.w,
                minimizers_seq_scalar(seq, hasher, self.w, &mut cache.0),
                min_pos,
            ),
            (false, true, 0) => collect_and_dedup_into_scalar(
                canonical_minimizers_seq_scalar(seq, hasher, self.w, &mut cache.0),
                min_pos,
            ),
            (false, true, 1) => collect_syncmers_scalar::<false>(
                self.w,
                canonical_minimizers_seq_scalar(seq, hasher, self.w, &mut cache.0),
                min_pos,
            ),
            (false, true, 2) => collect_syncmers_scalar::<true>(
                self.w,
                canonical_minimizers_seq_scalar(seq, hasher, self.w, &mut cache.0),
                min_pos,
            ),
            (true, false, 0) => minimizers_seq_simd(seq, hasher, self.w, &mut cache.0)
                .collect_and_dedup_into::<false>(min_pos),
            (true, false, 1) => minimizers_seq_simd(seq, hasher, self.w, &mut cache.0)
                .collect_syncmers_into::<false>(self.w, min_pos),
            (true, false, 2) => minimizers_seq_simd(seq, hasher, self.w, &mut cache.0)
                .collect_syncmers_into::<true>(self.w, min_pos),
            (true, true, 0) => canonical_minimizers_seq_simd(seq, hasher, self.w, &mut cache.0)
                .collect_and_dedup_into::<false>(min_pos),
            (true, true, 1) => canonical_minimizers_seq_simd(seq, hasher, self.w, &mut cache.0)
                .collect_syncmers_into::<false>(self.w, min_pos),
            (true, true, 2) => canonical_minimizers_seq_simd(seq, hasher, self.w, &mut cache.0)
                .collect_syncmers_into::<true>(self.w, min_pos),
            _ => unreachable!("SYNCMERS generic must be 0 (no syncmers), 1 (closed syncmers), or 2 (open syncmers)."),
        });
        Output {
            len: if SYNCMERS != 0 {
                self.k + self.w - 1
            } else {
                self.k
            },
            seq,
            min_pos,
        }
    }
}

impl<'h, H: KmerHasher, const SYNCMERS: u8> Builder<'h, true, H, (), SYNCMERS> {
    pub fn run_skip_ambiguous_windows_once<'s>(&self, nseq: PackedNSeq<'s>) -> Vec<u32> {
        let mut min_pos = vec![];
        self.run_skip_ambiguous_windows(nseq, &mut min_pos);
        min_pos
    }
    pub fn run_skip_ambiguous_windows<'s, 'o>(
        &self,
        nseq: PackedNSeq<'s>,
        min_pos: &'o mut Vec<u32>,
    ) -> Output<'o, true, PackedSeq<'s>> {
        CACHE
            .with_borrow_mut(|cache| self.run_skip_ambiguous_windows_with_buf(nseq, min_pos, cache))
    }
    pub fn run_skip_ambiguous_windows_with_buf<'s, 'o>(
        &self,
        nseq: PackedNSeq<'s>,
        min_pos: &'o mut Vec<u32>,
        cache: &mut (Cache, Vec<S>, Vec<S>),
    ) -> Output<'o, true, PackedSeq<'s>> {
        let default_hasher = self.hasher.is_none().then(|| H::new(self.k));
        let hasher = self
            .hasher
            .unwrap_or_else(|| default_hasher.as_ref().unwrap());
        match SYNCMERS {
            0 => canonical_minimizers_skip_ambiguous_windows(nseq, hasher, self.w, cache)
                .collect_and_dedup_into::<true>(min_pos),
            1 => canonical_minimizers_skip_ambiguous_windows(nseq, hasher, self.w, cache)
                .collect_syncmers_into::<false>(self.w, min_pos),
            2 => canonical_minimizers_skip_ambiguous_windows(nseq, hasher, self.w, cache)
                .collect_syncmers_into::<true>(self.w, min_pos),
            _ => panic!(
                "SYNCMERS generic must be 0 (no syncmers), 1 (closed syncmers), or 2 (open syncmers)."
            ),
        }
        Output {
            len: if SYNCMERS != 0 {
                self.k + self.w - 1
            } else {
                self.k
            },
            seq: nseq.seq,
            min_pos,
        }
    }
}

/// With-superkmer version
///
/// (does not work in combination with syncmers)
impl<'h, 'o2, const CANONICAL: bool, H: KmerHasher>
    Builder<'h, CANONICAL, H, &'o2 mut Vec<u32>, 0>
{
    pub fn run_scalar_once<'s, SEQ: Seq<'s>>(self, seq: SEQ) -> Vec<u32> {
        let mut min_pos = vec![];
        self.run_scalar(seq, &mut min_pos);
        min_pos
    }

    pub fn run_scalar<'s, 'o, SEQ: Seq<'s>>(
        self,
        seq: SEQ,
        min_pos: &'o mut Vec<u32>,
    ) -> Output<'o, CANONICAL, SEQ> {
        let default_hasher = self.hasher.is_none().then(|| H::new(self.k));
        let hasher = self
            .hasher
            .unwrap_or_else(|| default_hasher.as_ref().unwrap());

        CACHE.with_borrow_mut(|cache| match CANONICAL {
            false => collect_and_dedup_with_index_into_scalar(
                minimizers_seq_scalar(seq, hasher, self.w, &mut cache.0),
                min_pos,
                self.sk_pos,
            ),
            true => collect_and_dedup_with_index_into_scalar(
                canonical_minimizers_seq_scalar(seq, hasher, self.w, &mut cache.0),
                min_pos,
                self.sk_pos,
            ),
        });
        Output {
            len: self.k,
            seq,
            min_pos,
        }
    }

    pub fn run_once<'s, SEQ: Seq<'s>>(self, seq: SEQ) -> Vec<u32> {
        let mut min_pos = vec![];
        self.run(seq, &mut min_pos);
        min_pos
    }

    pub fn run<'s, 'o, SEQ: Seq<'s>>(
        self,
        seq: SEQ,
        min_pos: &'o mut Vec<u32>,
    ) -> Output<'o, CANONICAL, SEQ> {
        CACHE.with_borrow_mut(|cache| self.run_with_buf(seq, min_pos, &mut cache.0))
    }

    #[inline(always)]
    fn run_with_buf<'s, 'o, SEQ: Seq<'s>>(
        self,
        seq: SEQ,
        min_pos: &'o mut Vec<u32>,
        cache: &mut Cache,
    ) -> Output<'o, CANONICAL, SEQ> {
        let default_hasher = self.hasher.is_none().then(|| H::new(self.k));
        let hasher = self
            .hasher
            .unwrap_or_else(|| default_hasher.as_ref().unwrap());

        match CANONICAL {
            false => minimizers_seq_simd(seq, hasher, self.w, cache)
                .collect_and_dedup_with_index_into(min_pos, self.sk_pos),
            true => canonical_minimizers_seq_simd(seq, hasher, self.w, cache)
                .collect_and_dedup_with_index_into(min_pos, self.sk_pos),
        };
        Output {
            len: self.k,
            seq,
            min_pos,
        }
    }
}

impl<'s, 'o, const CANONICAL: bool, SEQ: Seq<'s>> Output<'o, CANONICAL, SEQ> {
    /// Iterator over (canonical) u64 kmer-values associated with all minimizer positions.
    #[must_use]
    pub fn values_u64(&self) -> impl ExactSizeIterator<Item = u64> {
        self.pos_and_values_u64().map(|(_pos, val)| val)
    }
    /// Iterator over (canonical) u128 kmer-values associated with all minimizer positions.
    #[must_use]
    pub fn values_u128(&self) -> impl ExactSizeIterator<Item = u128> {
        self.pos_and_values_u128().map(|(_pos, val)| val)
    }
    /// Iterator over positions and (canonical) u64 kmer-values associated with all minimizer positions.
    #[must_use]
    pub fn pos_and_values_u64(&self) -> impl ExactSizeIterator<Item = (u32, u64)> {
        self.min_pos.iter().map(
            #[inline(always)]
            move |&pos| {
                let val = if CANONICAL {
                    let a = self.seq.read_kmer(self.len, pos as usize);
                    let b = self.seq.read_revcomp_kmer(self.len, pos as usize);
                    core::cmp::min(a, b)
                } else {
                    self.seq.read_kmer(self.len, pos as usize)
                };
                (pos, val)
            },
        )
    }
    /// Iterator over positions and (canonical) u128 kmer-values associated with all minimizer positions.
    #[must_use]
    pub fn pos_and_values_u128(&self) -> impl ExactSizeIterator<Item = (u32, u128)> {
        self.min_pos.iter().map(
            #[inline(always)]
            move |&pos| {
                let val = if CANONICAL {
                    let a = self.seq.read_kmer_u128(self.len, pos as usize);
                    let b = self.seq.read_revcomp_kmer_u128(self.len, pos as usize);
                    core::cmp::min(a, b)
                } else {
                    self.seq.read_kmer_u128(self.len, pos as usize)
                };
                (pos, val)
            },
        )
    }
}

/// Positions of all minimizers in the sequence.
///
/// See [`minimizers`], [`canonical_minimizers`], and [`Builder`] for more
/// configurations supporting a custom hasher, super-kmer positions, and
/// returning kmer-values.
///
/// Positions are appended to a reusable `min_pos` vector to avoid allocations.
pub fn minimizer_positions<'s>(seq: impl Seq<'s>, k: usize, w: usize) -> Vec<u32> {
    minimizers(k, w).run_once(seq)
}

/// Positions of all canonical minimizers in the sequence.
///
/// See [`minimizers`], [`canonical_minimizers`], and [`Builder`] for more
/// configurations supporting a custom hasher, super-kmer positions, and
/// returning kmer-values.
///
/// `l=w+k-1` must be odd to determine the strand of each window.
///
/// Positions are appended to a reusable `min_pos` vector to avoid allocations.
pub fn canonical_minimizer_positions<'s>(seq: impl Seq<'s>, k: usize, w: usize) -> Vec<u32> {
    canonical_minimizers(k, w).run_once(seq)
}
