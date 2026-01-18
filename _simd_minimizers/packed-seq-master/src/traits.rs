use crate::{ChunkIt, PaddedIt};

use super::u32x8;
use mem_dbg::{MemDbg, MemSize};
use std::ops::Range;

/// Strong type indicating the delay passed to [`Seq::par_iter_bp_delayed`] and [`Seq::par_iter_bp_delayed_2`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Delay(pub usize);

/// A non-owned slice of characters.
///
/// The represented character values are expected to be in `[0, 2^b)`,
/// but they can be encoded in various ways. E.g.:
/// - A `&[u8]` of ASCII characters, returning 8-bit values.
/// - An `AsciiSeq` of DNA characters `ACGT`, interpreted 2-bit values.
/// - A `PackedSeq` of packed DNA characters (4 per byte), returning 2-bit values.
///
/// Each character is assumed to fit in 8 bits. Some functions take or return
/// this 'unpacked' (ASCII) character.
pub trait Seq<'s>: Copy + Eq + Ord {
    /// Number of bits `b` to represent each character returned by `iter_bp` and variants..
    const BITS_PER_CHAR: usize;
    /// Number of encoded characters per byte of memory of the `Seq`.
    const BASES_PER_BYTE: usize;

    /// The corresponding owned sequence type.
    type SeqVec: SeqVec;

    /// Convenience function that returns `b=Self::BITS_PER_CHAR`.
    fn bits_per_char(&self) -> usize {
        Self::BITS_PER_CHAR
    }

    /// The length of the sequence in characters.
    fn len(&self) -> usize;

    /// Returns `true` if the sequence is empty.
    fn is_empty(&self) -> bool;

    /// Get the character at the given index.
    fn get(&self, _index: usize) -> u8;

    /// Get the ASCII character at the given index, _without_ mapping to `b`-bit values.
    fn get_ascii(&self, _index: usize) -> u8;

    /// Convert a short sequence (kmer) to a packed representation as `u64`.
    fn as_u64(&self) -> u64;

    /// Convert a short sequence (kmer) to a packed representation of its reverse complement as `u64`.
    fn revcomp_as_u64(&self) -> u64;

    /// Convert a short sequence (kmer) to a packed representation as `u128`.
    fn as_u128(&self) -> u128;

    /// Convert a short sequence (kmer) to a packed representation of its reverse complement as `u128`.
    fn revcomp_as_u128(&self) -> u128;

    /// Convert a short sequence (kmer) to a packed representation as `usize`.
    #[deprecated = "Prefer `to_u64`."]
    #[inline(always)]
    fn to_word(&self) -> usize {
        self.as_u64() as usize
    }

    /// Convert a short sequence (kmer) to a packed representation of its reverse complement as `usize`.
    #[deprecated = "Prefer `revcomp_to_u64`."]
    #[inline(always)]
    fn to_word_revcomp(&self) -> usize {
        self.revcomp_as_u64() as usize
    }

    /// Convert to an owned version.
    fn to_vec(&self) -> Self::SeqVec;

    /// Compute the reverse complement of this sequence.
    fn to_revcomp(&self) -> Self::SeqVec;

    /// Get a sub-slice of the sequence.
    /// `range` indicates character indices.
    fn slice(&self, range: Range<usize>) -> Self;

    /// Extract a k-mer from this sequence.
    #[inline(always)]
    fn read_kmer(&self, k: usize, pos: usize) -> u64 {
        self.slice(pos..pos + k).as_u64()
    }

    /// Extract a reverse complement k-mer from this sequence.
    #[inline(always)]
    fn read_revcomp_kmer(&self, k: usize, pos: usize) -> u64 {
        self.slice(pos..pos + k).revcomp_as_u64()
    }

    /// Extract a k-mer from this sequence.
    #[inline(always)]
    fn read_kmer_u128(&self, k: usize, pos: usize) -> u128 {
        self.slice(pos..pos + k).as_u128()
    }

    /// Extract a reverse complement k-mer from this sequence.
    #[inline(always)]
    fn read_revcomp_kmer_u128(&self, k: usize, pos: usize) -> u128 {
        self.slice(pos..pos + k).revcomp_as_u128()
    }

    /// Iterate over the `b`-bit characters of the sequence.
    fn iter_bp(self) -> impl ExactSizeIterator<Item = u8>;

    /// Iterate over 8 chunks of `b`-bit characters of the sequence in parallel.
    ///
    /// This splits the input into 8 chunks and streams over them in parallel.
    /// The second output returns the number of 'padding' characters that was added to get a full number of SIMD lanes.
    /// Thus, the last `padding` number of returned elements (from the last lane(s)) should be ignored.
    /// The context can be e.g. the k-mer size being iterated.
    /// When `context>1`, consecutive chunks overlap by `context-1` bases.
    ///
    /// Expected to be implemented using SIMD instructions.
    fn par_iter_bp(self, context: usize) -> PaddedIt<impl ChunkIt<u32x8>>;

    /// Iterate over 8 chunks of the sequence in parallel, returning two characters offset by `delay` positions.
    ///
    /// Returned pairs are `(add, remove)`, and the first `delay` 'remove' characters are always `0`.
    ///
    /// For example, when the sequence starts as `ABCDEF...`, and `delay=2`,
    /// the first returned tuples in the first lane are:
    /// `(b'A', 0)`, `(b'B', 0)`, `(b'C', b'A')`, `(b'D', b'B')`.
    ///
    /// When `context>1`, consecutive chunks overlap by `context-1` bases:
    /// the first `context-1` 'added' characters of the second chunk overlap
    /// with the last `context-1` 'added' characters of the first chunk.
    fn par_iter_bp_delayed(
        self,
        context: usize,
        delay: Delay,
    ) -> PaddedIt<impl ChunkIt<(u32x8, u32x8)>>;

    /// Iterate over 8 chunks of the sequence in parallel, returning three characters:
    /// the char added, the one `delay` positions before, and the one `delay2` positions before.
    ///
    /// Requires `delay1 <= delay2`.
    ///
    /// Returned pairs are `(add, d1, d2)`. The first `delay1` `d1` characters and first `delay2` `d2` are always `0`.
    ///
    /// For example, when the sequence starts as `ABCDEF...`, and `delay1=2` and `delay2=3`,
    /// the first returned tuples in the first lane are:
    /// `(b'A', 0, 0)`, `(b'B', 0, 0)`, `(b'C', b'A', 0)`, `(b'D', b'B', b'A')`.
    ///
    /// When `context>1`, consecutive chunks overlap by `context-1` bases:
    /// the first `context-1` 'added' characters of the second chunk overlap
    /// with the last `context-1` 'added' characters of the first chunk.
    fn par_iter_bp_delayed_2(
        self,
        context: usize,
        delay1: Delay,
        delay2: Delay,
    ) -> PaddedIt<impl ChunkIt<(u32x8, u32x8, u32x8)>>;

    /// Compare and return the LCP of the two sequences.
    fn cmp_lcp(&self, other: &Self) -> (std::cmp::Ordering, usize);
}

// Some hacky stuff to make conditional supertraits.
cfg_if::cfg_if! {
    if #[cfg(feature = "epserde")] {
        pub use epserde::{deser::DeserializeInner, ser::SerializeInner};
    } else {
        pub trait SerializeInner {}
        pub trait DeserializeInner {}

        use crate::packed_seq::{Bits, SupportedBits, PackedSeqVecBase};

        impl SerializeInner for Vec<u8> {}
        impl DeserializeInner for Vec<u8> {}
        impl SerializeInner for crate::AsciiSeqVec {}
        impl DeserializeInner for crate::AsciiSeqVec {}
        impl<const B: usize> SerializeInner for PackedSeqVecBase<B> where Bits<B>:SupportedBits {}
        impl<const B: usize> DeserializeInner for PackedSeqVecBase<B> where Bits<B>:SupportedBits {}
    }
}

/// An owned sequence.
/// Can be constructed from either ASCII input or the underlying non-owning `Seq` type.
///
/// Implemented for:
/// - A `Vec<u8>` of ASCII characters, returning 8-bit values.
/// - An `AsciiSeqVec` of DNA characters `ACGT`, interpreted as 2-bit values.
/// - A `PackedSeqVec` of packed DNA characters (4 per byte), returning 2-bit values.
pub trait SeqVec:
    Default + Sync + SerializeInner + DeserializeInner + MemSize + MemDbg + Clone + 'static
{
    type Seq<'s>: Seq<'s>;

    /// Get a non-owning slice to the underlying sequence.
    ///
    /// Unfortunately, `Deref` into a `Seq` can not be supported.
    fn as_slice(&self) -> Self::Seq<'_>;

    /// Get a sub-slice of the sequence. Indices are character offsets.
    #[inline(always)]
    fn slice(&self, range: Range<usize>) -> Self::Seq<'_> {
        self.as_slice().slice(range)
    }

    /// Extract a k-mer from this sequence.
    #[inline(always)]
    fn read_kmer(&self, k: usize, pos: usize) -> u64 {
        self.as_slice().read_kmer(k, pos)
    }

    /// Extract a k-mer from this sequence.
    #[inline(always)]
    fn read_revcomp_kmer(&self, k: usize, pos: usize) -> u64 {
        self.as_slice().read_revcomp_kmer(k, pos)
    }

    /// Extract a k-mer from this sequence.
    #[inline(always)]
    fn read_kmer_u128(&self, k: usize, pos: usize) -> u128 {
        self.as_slice().read_kmer_u128(k, pos)
    }

    /// Extract a k-mer from this sequence.
    #[inline(always)]
    fn read_revcomp_kmer_u128(&self, k: usize, pos: usize) -> u128 {
        self.as_slice().read_revcomp_kmer_u128(k, pos)
    }

    /// The length of the sequence in characters.
    fn len(&self) -> usize;

    /// Returns `true` if the sequence is empty.
    fn is_empty(&self) -> bool;

    /// Empty the sequence.
    fn clear(&mut self);

    /// Convert into the underlying raw representation.
    fn into_raw(self) -> Vec<u8>;

    /// Generate a random sequence with the given number of characters.
    #[cfg(feature = "rand")]
    fn random(n: usize) -> Self;

    /// Create a `SeqVec` from ASCII input.
    #[inline(always)]
    fn from_ascii(seq: &[u8]) -> Self {
        let mut packed_vec = Self::default();
        packed_vec.push_ascii(seq);
        packed_vec
    }

    /// Append the given sequence to the underlying storage.
    ///
    /// This may leave gaps (padding) between consecutively pushed sequences to avoid re-aligning the pushed data.
    /// Returns the range of indices corresponding to the pushed sequence.
    /// Use `self.slice(range)` to get the corresponding slice.
    fn push_seq(&mut self, seq: Self::Seq<'_>) -> Range<usize>;

    /// Append the given ASCII sequence to the underlying storage.
    ///
    /// This may leave gaps (padding) between consecutively pushed sequences to avoid re-aligning the pushed data.
    /// Returns the range of indices corresponding to the pushed sequence.
    /// Use `self.slice(range)` to get the corresponding slice.
    fn push_ascii(&mut self, seq: &[u8]) -> Range<usize>;
}
