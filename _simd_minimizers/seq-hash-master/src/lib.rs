//! A crate for streaming hashing of k-mers via `KmerHasher`.
//!
//! This builds on [`packed_seq`] and is used by e.g. [`simd_minimizers`].
//!
//! The default [`NtHasher`] is canonical.
//! If that's not needed, [`NtHasher<false>`] will be slightly faster.
//! For non-DNA sequences with >2-bit alphabets, use [`MulHasher`] instead.
//!
//! Note that [`KmerHasher`] objects need `k` on their construction, so that they can precompute required constants.
//! Prefer reusing the same [`KmerHasher`].
//!
//! This crate also includes [`AntiLexHasher`], see [this blogpost](https://curiouscoding.nl/posts/practical-minimizers/).
//!
//! ## Typical usage
//!
//! Construct a default [`NtHasher`] via `let hasher = <NtHasher>::new(k)`.
//! Then call either `hasher.hash_kmers_simd(seq, context)`,
//! or use the underlying 'mapper' via `hasher.in_out_mapper_simd(seq)`.
//! ```
//! use packed_seq::{AsciiSeqVec, PackedSeqVec, SeqVec};
//! use seq_hash::{KmerHasher, NtHasher};
//! let k = 3;
//!
//! // Default `NtHasher` is canonical.
//! let hasher = <NtHasher>::new(k);
//! let kmer = PackedSeqVec::from_ascii(b"ACG");
//! let kmer_rc = PackedSeqVec::from_ascii(b"CGT");
//! // Normally, prefer `hash_kmers_simd` over `hash_seq`.
//! assert_eq!(
//!     hasher.hash_seq(kmer.as_slice()),
//!     hasher.hash_seq(kmer_rc.as_slice())
//! );
//!
//! let fwd_hasher = NtHasher::<false>::new(k);
//! assert_ne!(
//!     fwd_hasher.hash_seq(kmer.as_slice()),
//!     fwd_hasher.hash_seq(kmer_rc.as_slice())
//! );
//!
//! let seq = b"ACGGCAGCGCATATGTAGT";
//! let ascii_seq = AsciiSeqVec::from_ascii(seq);
//! let packed_seq = PackedSeqVec::from_ascii(seq);
//!
//! // hasher.hash_kmers_scalar(seq.as_slice()); // Panics since `NtHasher` does not support ASCII.
//! let hashes_1: Vec<_> = hasher.hash_kmers_scalar(ascii_seq.as_slice()).collect();
//! let hashes_2: Vec<_> = hasher.hash_kmers_scalar(packed_seq.as_slice()).collect();
//! // Hashes are equal for [`packed_seq::AsciiSeq`] and [`packed_seq::PackedSeq`].
//! assert_eq!(hashes_1, hashes_2);
//! assert_eq!(hashes_1.len(), seq.len() - (k-1));
//!
//! // Consider a 'context' of a single kmer.
//! let hashes_3: Vec<_> = hasher.hash_kmers_simd(ascii_seq.as_slice(), 1).collect();
//! let hashes_4: Vec<_> = hasher.hash_kmers_simd(packed_seq.as_slice(), 1).collect();
//! assert_eq!(hashes_1, hashes_3);
//! assert_eq!(hashes_1, hashes_4);
//! ```

mod anti_lex;
mod intrinsics;
mod nthash;
#[cfg(test)]
mod test;

pub use anti_lex::AntiLexHasher;
pub use nthash::{MulHasher, NtHasher};

/// Re-export of the `packed-seq` crate.
pub use packed_seq;

use packed_seq::{ChunkIt, Delay, PackedNSeq, PaddedIt, Seq};
use std::iter::{repeat, zip};

type S = wide::u32x8;

/// A hasher that can hash all k-mers in a string.
///
/// Note that a `KmerHasher` must be initialized with a specific `k`,
/// so that it can precompute associated constants.
pub trait KmerHasher {
    /// True when the hash function is invariant under reverse-complement.
    const CANONICAL: bool;

    fn new(k: usize) -> Self;

    /// Helper function returning [`Self::CANONICAL`].
    #[inline(always)]
    fn is_canonical(&self) -> bool {
        Self::CANONICAL
    }

    /// The value of `k` for this hasher.
    fn k(&self) -> usize;

    /// The delay of the 'out' character passed to the `in_out_mapper` functions.
    /// Defaults to `k-1`.
    #[inline(always)]
    fn delay(&self) -> Delay {
        Delay(self.k() - 1)
    }

    /// A scalar mapper function that should be called with each `(in, out)` base.
    ///
    /// The delay should be [`Self::delay()`]. The first `delay` calls should have `out=0`.
    /// `seq` is only used to ensure that the hasher can handle the underlying alphabet.
    fn in_out_mapper_scalar<'s>(&self, seq: impl Seq<'s>) -> impl FnMut((u8, u8)) -> u32;
    /// A SIMD mapper function that should be called with a `(in, out)` base per lane.
    ///
    /// The delay should be [`Self::delay()`]. The first `delay` calls should have `out=u32x8::splat(0)`.
    /// `seq` is only used to ensure that the hasher can handle the underlying alphabet.
    fn in_out_mapper_simd<'s>(&self, seq: impl Seq<'s>) -> impl FnMut((S, S)) -> S;

    fn in_out_mapper_ambiguous_scalar<'s>(
        &self,
        nseq: PackedNSeq<'s>,
    ) -> impl FnMut((u8, u8)) -> u32 {
        let mut mapper = self.in_out_mapper_scalar(nseq.seq);
        let mut ambiguous = nseq.ambiguous.iter_kmer_ambiguity(self.k());
        let k = self.k();
        let mut i = 0;
        move |(a, r)| {
            let hash = mapper((a, r));
            let ambiguous = if i > k - 1 {
                ambiguous.next().unwrap()
            } else {
                false
            };
            i += 1;
            if ambiguous { u32::MAX } else { hash }
        }
    }

    #[inline(always)]
    fn in_out_mapper_ambiguous_simd<'s>(
        &self,
        nseq: PackedNSeq<'s>,
        context: usize,
    ) -> impl FnMut((S, S)) -> S {
        let mut mapper = self.in_out_mapper_simd(nseq.seq);
        let mut ambiguous = nseq.ambiguous.par_iter_kmer_ambiguity(self.k(), context, 0);
        move |(a, r)| {
            let hash = mapper((a, r));
            let ambiguous = ambiguous.it.next().unwrap();
            ambiguous.blend(S::MAX, hash)
        }
    }

    /// A scalar iterator over all k-mer hashes in `seq`.
    #[inline(always)]
    fn hash_kmers_scalar<'s>(&self, seq: impl Seq<'s>) -> impl ExactSizeIterator<Item = u32> {
        let k = self.k();
        let delay = self.delay();
        let mut add = seq.iter_bp();
        let mut remove = seq.iter_bp();
        let mut mapper = self.in_out_mapper_scalar(seq);
        zip(add.by_ref().take(delay.0), repeat(0)).for_each(|a| {
            mapper(a);
        });
        zip(add.by_ref(), remove.by_ref())
            .take(k - 1 - delay.0)
            .for_each(|a| {
                mapper(a);
            });
        zip(add, remove).map(mapper)
    }

    /// A SIMD-parallel iterator over all k-mer hashes in `seq`.
    #[inline(always)]
    fn hash_kmers_simd<'s>(&self, seq: impl Seq<'s>, context: usize) -> PaddedIt<impl ChunkIt<S>> {
        let k = self.k();
        let delay = self.delay();
        seq.par_iter_bp_delayed(context + k - 1, delay)
            .map(self.in_out_mapper_simd(seq))
            .advance(k - 1)
    }

    /// An iterator over all k-mer hashes in `seq`.
    /// Ambiguous kmers get hash `u32::MAX`.
    #[inline(always)]
    fn hash_valid_kmers_scalar<'s>(
        &self,
        nseq: PackedNSeq<'s>,
    ) -> impl ExactSizeIterator<Item = u32> {
        let k = self.k();
        let delay = self.delay();
        assert!(delay.0 < k);

        let mut mapper = self.in_out_mapper_scalar(nseq.seq);

        let mut a = nseq.seq.iter_bp();
        let mut r = nseq.seq.iter_bp();

        a.by_ref().take(delay.0).for_each(
            #[inline(always)]
            |a| {
                mapper((a, 0));
            },
        );

        zip(a.by_ref(), r.by_ref())
            .take((k - 1) - delay.0)
            .for_each(
                #[inline(always)]
                |(a, r)| {
                    mapper((a, r));
                },
            );

        zip(zip(a, r), nseq.ambiguous.iter_kmer_ambiguity(k)).map(
            #[inline(always)]
            move |(ar, ambiguous)| {
                let hash = mapper(ar);
                if ambiguous { u32::MAX } else { hash }
            },
        )
    }

    /// A SIMD-parallel iterator over all k-mer hashes in `seq`.
    /// Ambiguous kmers get hash `u32::MAX`.
    #[inline(always)]
    fn hash_valid_kmers_simd<'s, 't>(
        &'t self,
        nseq: PackedNSeq<'s>,
        context: usize,
    ) -> PaddedIt<impl ChunkIt<S> + use<'s, 't, Self>> {
        let k = self.k();
        let delay = self.delay();
        let mut hash_mapper = self.in_out_mapper_simd(nseq.seq);
        let mut ambiguity_it = nseq
            .ambiguous
            .par_iter_kmer_ambiguity(k, context + k - 1, 0);
        nseq.seq
            .par_iter_bp_delayed_with_factor(context + k - 1, delay, 2)
            .map(
                #[inline(always)]
                move |(a, r)| {
                    let is_ambiguous = ambiguity_it.it.next().unwrap();
                    let hash = hash_mapper((a, r));
                    is_ambiguous.blend(S::MAX, hash)
                },
            )
            .advance(k - 1)
    }

    /// Hash a sequence one character at a time. Ignores `k`.
    ///
    /// `seq` is only used to ensure that the hasher can handle the underlying alphabet.
    fn mapper<'s>(&self, seq: impl Seq<'s>) -> impl FnMut(u8) -> u32;

    /// Hash the given sequence. Ignores `k`.
    ///
    /// This is slightly inefficient because it recomputes the constants based on the sequence length.
    #[inline(always)]
    fn hash_seq<'s>(&self, seq: impl Seq<'s>) -> u32 {
        seq.iter_bp().map(self.mapper(seq)).last().unwrap_or(0)
    }
    /// Hash all non-empty prefixes of the given sequence. Ignores `k`.
    #[inline(always)]
    fn hash_prefixes<'s>(&self, seq: impl Seq<'s>) -> impl ExactSizeIterator<Item = u32> {
        seq.iter_bp().map(self.mapper(seq))
    }
}
