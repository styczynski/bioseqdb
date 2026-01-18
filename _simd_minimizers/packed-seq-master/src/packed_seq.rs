use core::cell::RefCell;
use std::ops::{Deref, DerefMut};
use traits::Seq;
use wide::u16x8;

use crate::{intrinsics::transpose, padded_it::ChunkIt};

use super::*;

type SimdBuf = [S; 8];

struct RecycledBox(Option<Box<SimdBuf>>);

thread_local! {
    static RECYCLED_BOX_CACHE: RefCell<Vec<Box<SimdBuf>>> = {
        RefCell::new(vec![Box::new(SimdBuf::default())])
    };
}

impl RecycledBox {
    #[inline(always)]
    fn take() -> Self {
        let mut buf = RECYCLED_BOX_CACHE.with_borrow_mut(|v| RecycledBox(v.pop()));
        buf.init_if_needed();
        buf
    }

    #[inline(always)]
    fn init_if_needed(&mut self) {
        if self.0.is_none() {
            self.0 = Some(Box::new(SimdBuf::default()));
        }
    }

    #[inline(always)]
    fn get(&self) -> &SimdBuf {
        unsafe { self.0.as_ref().unwrap_unchecked() }
    }

    #[inline(always)]
    fn get_mut(&mut self) -> &mut SimdBuf {
        unsafe { self.0.as_mut().unwrap_unchecked() }
    }
}

impl Deref for RecycledBox {
    type Target = SimdBuf;

    #[inline(always)]
    fn deref(&self) -> &Self::Target {
        self.get()
    }
}
impl DerefMut for RecycledBox {
    #[inline(always)]
    fn deref_mut(&mut self) -> &mut SimdBuf {
        self.get_mut()
    }
}

impl Drop for RecycledBox {
    #[inline(always)]
    fn drop(&mut self) {
        let mut x = None;
        core::mem::swap(&mut x, &mut self.0);
        RECYCLED_BOX_CACHE.with_borrow_mut(|v| v.push(unsafe { x.unwrap_unchecked() }));
    }
}

struct RecycledVec(Vec<S>);

thread_local! {
    static RECYCLED_VEC_CACHE: RefCell<Vec<Vec<S>>> = {
        RefCell::new(vec![])
    };
}

impl RecycledVec {
    #[inline(always)]
    fn take() -> Self {
        RecycledVec(RECYCLED_VEC_CACHE.with_borrow_mut(|v| v.pop().unwrap_or_default()))
    }
}

impl Deref for RecycledVec {
    type Target = Vec<S>;
    #[inline(always)]
    fn deref(&self) -> &Self::Target {
        &self.0
    }
}
impl DerefMut for RecycledVec {
    #[inline(always)]
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}
impl Drop for RecycledVec {
    #[inline(always)]
    fn drop(&mut self) {
        RECYCLED_VEC_CACHE.with_borrow_mut(|v| v.push(std::mem::take(&mut self.0)));
    }
}

#[doc(hidden)]
pub struct Bits<const B: usize>;
#[doc(hidden)]
pub trait SupportedBits {}
impl SupportedBits for Bits<1> {}
impl SupportedBits for Bits<2> {}
impl SupportedBits for Bits<4> {}
impl SupportedBits for Bits<8> {}

/// Number of padding bytes at the end of `PackedSeqVecBase::seq`.
pub(crate) const PADDING: usize = 48;

/// A variable-bit-width packed non-owned slice of DNA bases.
#[derive(Copy, Clone, Debug, MemSize, MemDbg)]
pub struct PackedSeqBase<'s, const B: usize>
where
    Bits<B>: SupportedBits,
{
    /// Packed data.
    seq: &'s [u8],
    /// Offset in bp from the start of the `seq`.
    offset: usize,
    /// Length of the sequence in bp, starting at `offset` from the start of `seq`.
    len: usize,
}

/// A variable-bit-width packed owned sequence of DNA bases.
#[derive(Clone, Debug, MemSize, MemDbg)]
#[cfg_attr(feature = "pyo3", pyo3::pyclass)]
#[cfg_attr(feature = "epserde", derive(epserde::Epserde))]
pub struct PackedSeqVecBase<const B: usize>
where
    Bits<B>: SupportedBits,
{
    /// NOTE: We maintain the invariant that this has at least 48 bytes of padding
    /// at the end after `len` finishes.
    /// This ensures that `read_unaligned` in `as_64` works OK.
    pub(crate) seq: Vec<u8>,

    /// The length, in bp, of the underlying sequence. See `.len()`.
    len: usize,
}

pub type PackedSeq<'s> = PackedSeqBase<'s, 2>;
pub type PackedSeqVec = PackedSeqVecBase<2>;
pub type BitSeq<'s> = PackedSeqBase<'s, 1>;
pub type BitSeqVec = PackedSeqVecBase<1>;

/// Convenience constants.
/// B: bits per chat
impl<'s, const B: usize> PackedSeqBase<'s, B>
where
    Bits<B>: SupportedBits,
{
    /// lowest B bits are 1.
    const CHAR_MASK: u64 = (1 << B) - 1;
    const SIMD_B: S = S::new([B as u32; 8]);
    const SIMD_CHAR_MASK: S = S::new([(1 << B) - 1; 8]);
    /// Chars per byte
    const C8: usize = 8 / B;
    /// Chars per u32
    const C32: usize = 32 / B;
    /// Chars per u256
    const C256: usize = 256 / B;
    /// Max length of a kmer that can be read as a single u64.
    const K64: usize = (64 - 8) / B + 1;
}

/// Convenience constants.
impl<const B: usize> PackedSeqVecBase<B>
where
    Bits<B>: SupportedBits,
{
    /// Chars per byte
    const C8: usize = 8 / B;
}

impl<const B: usize> Default for PackedSeqVecBase<B>
where
    Bits<B>: SupportedBits,
{
    fn default() -> Self {
        Self {
            seq: vec![0; PADDING],
            len: 0,
        }
    }
}

// ======================================================================
// 2-BIT HELPER METHODS

/// Pack an ASCII `ACTGactg` character into its 2-bit representation, and panic for anything else.
#[inline(always)]
pub fn pack_char(base: u8) -> u8 {
    match base {
        b'a' | b'A' => 0,
        b'c' | b'C' => 1,
        b'g' | b'G' => 3,
        b't' | b'T' => 2,
        _ => panic!(
            "Unexpected character '{}' with ASCII value {base}. Expected one of ACTGactg.",
            base as char
        ),
    }
}

/// Pack an ASCII `ACTGactg` character into its 2-bit representation, and silently convert other characters into 0..4 as well.
#[inline(always)]
pub fn pack_char_lossy(base: u8) -> u8 {
    (base >> 1) & 3
}
/// Pack a slice of ASCII `ACTGactg` characters into its packed 2-bit kmer representation.
/// Other characters are silently converted.
#[inline(always)]
pub fn pack_kmer_lossy(slice: &[u8]) -> u64 {
    let mut kmer = 0;
    for (i, &base) in slice.iter().enumerate() {
        kmer |= (pack_char_lossy(base) as u64) << (2 * i);
    }
    kmer
}
/// Pack a slice of ASCII `ACTGactg` characters into its packed 2-bit kmer representation.
#[inline(always)]
pub fn pack_kmer_u128_lossy(slice: &[u8]) -> u128 {
    let mut kmer = 0;
    for (i, &base) in slice.iter().enumerate() {
        kmer |= (pack_char_lossy(base) as u128) << (2 * i);
    }
    kmer
}

/// Unpack a 2-bit DNA base into the corresponding `ACTG` character.
#[inline(always)]
pub fn unpack_base(base: u8) -> u8 {
    debug_assert!(base < 4, "Base {base} is not <4.");
    b"ACTG"[base as usize]
}

/// Unpack a 2-bit encoded kmer into corresponding `ACTG` characters.
/// Slice the returned array to the correct length.
#[inline(always)]
pub fn unpack_kmer(kmer: u64) -> [u8; 32] {
    std::array::from_fn(|i| unpack_base(((kmer >> (2 * i)) & 3) as u8))
}
/// Unpack a 2-bit encoded kmer into corresponding `ACTG` character.
#[inline(always)]
pub fn unpack_kmer_into_vec(kmer: u64, k: usize, out: &mut Vec<u8>) {
    out.clear();
    out.extend((0..k).map(|i| unpack_base(((kmer >> (2 * i)) & 3) as u8)));
}
/// Unpack a 2-bit encoded kmer into corresponding `ACTG` character.
#[inline(always)]
pub fn unpack_kmer_to_vec(kmer: u64, k: usize) -> Vec<u8> {
    let mut out = vec![];
    unpack_kmer_into_vec(kmer, k, &mut out);
    out
}

/// Unpack a 2-bit encoded kmer into corresponding `ACTG` characters.
/// Slice the returned array to the correct length.
#[inline(always)]
pub fn unpack_kmer_u128(kmer: u128) -> [u8; 64] {
    std::array::from_fn(|i| unpack_base(((kmer >> (2 * i)) & 3) as u8))
}
/// Unpack a 2-bit encoded kmer into corresponding `ACTG` character.
#[inline(always)]
pub fn unpack_kmer_u128_into_vec(kmer: u128, k: usize, out: &mut Vec<u8>) {
    out.clear();
    out.extend((0..k).map(|i| unpack_base(((kmer >> (2 * i)) & 3) as u8)));
}
/// Unpack a 2-bit encoded kmer into corresponding `ACTG` character.
#[inline(always)]
pub fn unpack_kmer_u128_to_vec(kmer: u128, k: usize) -> Vec<u8> {
    let mut out = vec![];
    unpack_kmer_u128_into_vec(kmer, k, &mut out);
    out
}

/// Complement an ASCII character: `A<>T` and `C<>G`.
#[inline(always)]
pub const fn complement_char(base: u8) -> u8 {
    match base {
        b'A' => b'T',
        b'C' => b'G',
        b'G' => b'C',
        b'T' => b'A',
        _ => panic!("Unexpected character. Expected one of ACTGactg.",),
    }
}

/// Complement a 2-bit base: `0<>2` and `1<>3`.
#[inline(always)]
pub const fn complement_base(base: u8) -> u8 {
    base ^ 2
}

/// Complement 8 lanes of 2-bit bases: `0<>2` and `1<>3`.
#[inline(always)]
pub fn complement_base_simd(base: u32x8) -> u32x8 {
    const TWO: u32x8 = u32x8::new([2; 8]);
    base ^ TWO
}

/// Reverse complement the 2-bit pairs in the input.
#[inline(always)]
const fn revcomp_raw(word: u64) -> u64 {
    #[cfg(any(target_arch = "arm", target_arch = "aarch64"))]
    {
        let mut res = word.reverse_bits(); // ARM can reverse bits in a single instruction
        res = ((res >> 1) & 0x5555_5555_5555_5555) | ((res & 0x5555_5555_5555_5555) << 1);
        res ^ 0xAAAA_AAAA_AAAA_AAAA
    }

    #[cfg(not(any(target_arch = "arm", target_arch = "aarch64")))]
    {
        let mut res = word.swap_bytes();
        res = ((res >> 4) & 0x0F0F_0F0F_0F0F_0F0F) | ((res & 0x0F0F_0F0F_0F0F_0F0F) << 4);
        res = ((res >> 2) & 0x3333_3333_3333_3333) | ((res & 0x3333_3333_3333_3333) << 2);
        res ^ 0xAAAA_AAAA_AAAA_AAAA
    }
}

/// Compute the reverse complement of a short sequence packed in a `u64`.
#[inline(always)]
pub const fn revcomp_u64(word: u64, len: usize) -> u64 {
    revcomp_raw(word) >> (usize::BITS as usize - 2 * len)
}

#[inline(always)]
pub const fn revcomp_u128(word: u128, len: usize) -> u128 {
    let low = word as u64;
    let high = (word >> 64) as u64;
    let rlow = revcomp_raw(low);
    let rhigh = revcomp_raw(high);
    let out = ((rlow as u128) << 64) | rhigh as u128;
    out >> (u128::BITS as usize - 2 * len)
}

// ======================================================================
// 1-BIT HELPER METHODS

/// 1 when a char is ambiguous.
#[inline(always)]
pub fn char_is_ambiguous(base: u8) -> u8 {
    // (!matches!(base, b'A' | b'C'  | b'G'  | b'T' | b'a' | b'c'  | b'g'  | b't')) as u8
    let table = b"ACTG";
    let upper_mask = !(b'a' - b'A');
    (table[pack_char_lossy(base) as usize] != (base & upper_mask)) as u8
}

/// Reverse `len` bits packed in a `u64`.
#[inline(always)]
pub const fn rev_u64(word: u64, len: usize) -> u64 {
    word.reverse_bits() >> (usize::BITS as usize - len)
}

/// Reverse `len` bits packed in a `u128`.
#[inline(always)]
pub const fn rev_u128(word: u128, len: usize) -> u128 {
    word.reverse_bits() >> (u128::BITS as usize - len)
}

// ======================================================================

impl<'s, const B: usize> PackedSeqBase<'s, B>
where
    Bits<B>: SupportedBits,
{
    /// Creates a `Seq` from a slice of packed bytes, an offset in bp and a length in bp.
    ///
    /// The slice should have at least 48 bytes of padding after `offset + len`.
    /// Otherwise, the function will panic.
    pub fn from_raw_parts(seq: &'s [u8], offset: usize, len: usize) -> Self {
        assert!(offset + len + PADDING * Self::C8 <= seq.len() * Self::C8);
        Self { seq, offset, len }
    }

    /// Shrink `seq` to only just cover the data.
    #[inline(always)]
    pub fn normalize(&self) -> Self {
        let start_byte = self.offset / Self::C8;
        let end_byte = (self.offset + self.len).div_ceil(Self::C8);
        Self {
            seq: &self.seq[start_byte..end_byte + PADDING],
            offset: self.offset % Self::C8,
            len: self.len,
        }
    }

    /// Return a `Vec<u8>` of ASCII `ACTG` characters.
    #[inline(always)]
    pub fn unpack(&self) -> Vec<u8> {
        self.iter_bp().map(unpack_base).collect()
    }
}

/// Read up to 32 bytes starting at idx.
#[inline(always)]
pub(crate) unsafe fn read_slice_32_unchecked(seq: &[u8], idx: usize) -> u32x8 {
    unsafe {
        let src = seq.as_ptr().add(idx);
        debug_assert!(idx + 32 <= seq.len());
        std::mem::transmute::<_, *const u32x8>(src).read_unaligned()
    }
}

/// Read up to 32 bytes starting at idx.
#[inline(always)]
pub(crate) fn read_slice_32(seq: &[u8], idx: usize) -> u32x8 {
    unsafe {
        let src = seq.as_ptr().add(idx);
        if idx + 32 <= seq.len() {
            std::mem::transmute::<_, *const u32x8>(src).read_unaligned()
        } else {
            let num_bytes = seq.len().saturating_sub(idx);
            let mut result = [0u8; 32];
            std::ptr::copy_nonoverlapping(src, result.as_mut_ptr(), num_bytes);
            std::mem::transmute(result)
        }
    }
}

/// Read up to 16 bytes starting at idx.
#[allow(unused)]
#[inline(always)]
pub(crate) fn read_slice_16(seq: &[u8], idx: usize) -> u16x8 {
    unsafe {
        let src = seq.as_ptr().add(idx);
        if idx + 16 <= seq.len() {
            std::mem::transmute::<_, *const u16x8>(src).read_unaligned()
        } else {
            let num_bytes = seq.len().saturating_sub(idx);
            let mut result = [0u8; 16];
            std::ptr::copy_nonoverlapping(src, result.as_mut_ptr(), num_bytes);
            std::mem::transmute(result)
        }
    }
}

impl<'s, const B: usize> Seq<'s> for PackedSeqBase<'s, B>
where
    Bits<B>: SupportedBits,
{
    const BITS_PER_CHAR: usize = B;
    const BASES_PER_BYTE: usize = Self::C8;
    type SeqVec = PackedSeqVecBase<B>;

    #[inline(always)]
    fn len(&self) -> usize {
        self.len
    }

    #[inline(always)]
    fn is_empty(&self) -> bool {
        self.len == 0
    }

    #[inline(always)]
    fn get_ascii(&self, index: usize) -> u8 {
        unpack_base(self.get(index))
    }

    /// Convert a short sequence (kmer) to a packed representation as `u64`.
    /// Panics if `self` is longer than 32 characters.
    #[inline(always)]
    fn as_u64(&self) -> u64 {
        assert!(self.len() <= 64 / B);

        let mask = u64::MAX >> (64 - B * self.len());

        // The unaligned read is OK, because we ensure that the underlying `PackedSeqVecBase::seq` always
        // has at least 48 bytes of padding at the end.
        if self.len() <= Self::K64 {
            let x = unsafe { (self.seq.as_ptr() as *const u64).read_unaligned() };
            (x >> (B * self.offset)) & mask
        } else {
            let x = unsafe { (self.seq.as_ptr() as *const u128).read_unaligned() };
            (x >> (B * self.offset)) as u64 & mask
        }
    }

    /// Convert a short sequence (kmer) to a packed representation of its reverse complement as `usize`.
    /// Panics if `self` is longer than 32 characters.
    #[inline(always)]
    fn revcomp_as_u64(&self) -> u64 {
        match B {
            1 => rev_u64(self.as_u64(), self.len()),
            2 => revcomp_u64(self.as_u64(), self.len()),
            _ => panic!("Rev(comp) is only supported for 1-bit and 2-bit alphabets."),
        }
    }

    /// Convert a short sequence (kmer) to a packed representation as `u128`.
    /// Panics if `self` is longer than 64 characters.
    #[inline(always)]
    fn as_u128(&self) -> u128 {
        assert!(
            self.len() <= (128 - 8) / B + 1,
            "Sequences >61 long cannot be read with a single unaligned u128 read."
        );

        let mask = u128::MAX >> (128 - B * self.len());

        // The unaligned read is OK, because we ensure that the underlying `PackedSeqVecBase::seq` always
        // has at least 48 bytes of padding at the end.
        let x = unsafe { (self.seq.as_ptr() as *const u128).read_unaligned() };
        (x >> (B * self.offset)) & mask
    }

    /// Convert a short sequence (kmer) to a packed representation of its reverse complement as `usize`.
    /// Panics if `self` is longer than 64 characters.
    #[inline(always)]
    fn revcomp_as_u128(&self) -> u128 {
        match B {
            1 => rev_u128(self.as_u128(), self.len()),
            2 => revcomp_u128(self.as_u128(), self.len()),
            _ => panic!("Rev(comp) is only supported for 1-bit and 2-bit alphabets."),
        }
    }

    #[inline(always)]
    fn to_vec(&self) -> PackedSeqVecBase<B> {
        assert_eq!(self.offset, 0);
        PackedSeqVecBase {
            seq: self
                .seq
                .iter()
                .copied()
                .chain(std::iter::repeat_n(0u8, PADDING))
                .collect(),
            len: self.len,
        }
    }

    fn to_revcomp(&self) -> PackedSeqVecBase<B> {
        match B {
            1 | 2 => {}
            _ => panic!("Can only reverse (&complement) 1-bit and 2-bit packed sequences.",),
        }

        let mut seq = self.seq[..(self.offset + self.len).div_ceil(Self::C8)]
            .iter()
            // 1. reverse the bytes
            .rev()
            .copied()
            .map(|mut res| {
                match B {
                    2 => {
                        // 2. swap the bases in the byte
                        // This is auto-vectorized.
                        res = ((res >> 4) & 0x0F) | ((res & 0x0F) << 4);
                        res = ((res >> 2) & 0x33) | ((res & 0x33) << 2);
                        // Complement the bases.
                        res ^ 0xAA
                    }
                    1 => res.reverse_bits(),
                    _ => unreachable!(),
                }
            })
            .chain(std::iter::repeat_n(0u8, PADDING))
            .collect::<Vec<u8>>();

        // 3. Shift away the offset.
        let new_offset = (Self::C8 - (self.offset + self.len) % Self::C8) % Self::C8;

        if new_offset > 0 {
            // Shift everything left by `2*new_offset` bits.
            let shift = B * new_offset;
            *seq.last_mut().unwrap() >>= shift;
            // This loop is also auto-vectorized.
            for i in 0..seq.len() - 1 {
                seq[i] = (seq[i] >> shift) | (seq[i + 1] << (8 - shift));
            }
        }

        PackedSeqVecBase { seq, len: self.len }
    }

    #[inline(always)]
    fn slice(&self, range: Range<usize>) -> Self {
        debug_assert!(
            range.end <= self.len,
            "Slice index out of bounds: {} > {}",
            range.end,
            self.len
        );
        PackedSeqBase {
            seq: self.seq,
            offset: self.offset + range.start,
            len: range.end - range.start,
        }
        .normalize()
    }

    #[inline(always)]
    fn iter_bp(self) -> impl ExactSizeIterator<Item = u8> {
        assert!(self.len <= self.seq.len() * Self::C8);

        let this = self.normalize();

        // read u64 at a time?
        let mut byte = 0;
        (0..this.len + this.offset)
            .map(
                #[inline(always)]
                move |i| {
                    if i % Self::C8 == 0 {
                        byte = this.seq[i / Self::C8];
                    }
                    // Shift byte instead of i?
                    (byte >> (B * (i % Self::C8))) & Self::CHAR_MASK as u8
                },
            )
            .advance(this.offset)
    }

    #[inline(always)]
    fn par_iter_bp(self, context: usize) -> PaddedIt<impl ChunkIt<S>> {
        self.par_iter_bp_with_buf(context, RecycledBox::take())
    }

    #[inline(always)]
    fn par_iter_bp_delayed(self, context: usize, delay: Delay) -> PaddedIt<impl ChunkIt<(S, S)>> {
        self.par_iter_bp_delayed_with_factor(context, delay, 1)
    }

    /// NOTE: When `self` starts does not start at a byte boundary, the
    /// 'delayed' character is not guaranteed to be `0`.
    #[inline(always)]
    fn par_iter_bp_delayed_2(
        self,
        context: usize,
        delay1: Delay,
        delay2: Delay,
    ) -> PaddedIt<impl ChunkIt<(S, S, S)>> {
        self.par_iter_bp_delayed_2_with_factor_and_buf(
            context,
            delay1,
            delay2,
            1,
            RecycledVec::take(),
        )
    }

    /// Compares 29 characters at a time.
    fn cmp_lcp(&self, other: &Self) -> (std::cmp::Ordering, usize) {
        let mut lcp = 0;
        let min_len = self.len.min(other.len);
        for i in (0..min_len).step_by(Self::K64) {
            let len = (min_len - i).min(Self::K64);
            let this = self.slice(i..i + len);
            let other = other.slice(i..i + len);
            let this_word = this.as_u64();
            let other_word = other.as_u64();
            if this_word != other_word {
                // Unfortunately, bases are packed in little endian order, so the default order is reversed.
                let eq = this_word ^ other_word;
                let t = eq.trailing_zeros() as usize / B * B;
                lcp += t / B;
                let mask = (Self::CHAR_MASK) << t;
                return ((this_word & mask).cmp(&(other_word & mask)), lcp);
            }
            lcp += len;
        }
        (self.len.cmp(&other.len), lcp)
    }

    #[inline(always)]
    fn get(&self, index: usize) -> u8 {
        let offset = self.offset + index;
        let idx = offset / Self::C8;
        let offset = offset % Self::C8;
        (self.seq[idx] >> (B * offset)) & Self::CHAR_MASK as u8
    }
}

impl<'s, const B: usize> PackedSeqBase<'s, B>
where
    Bits<B>: SupportedBits,
{
    #[inline(always)]
    pub fn par_iter_bp_with_buf<BUF: DerefMut<Target = [S; 8]>>(
        self,
        context: usize,
        mut buf: BUF,
    ) -> PaddedIt<impl ChunkIt<S> + use<'s, B, BUF>> {
        #[cfg(target_endian = "big")]
        panic!("Big endian architectures are not supported.");

        let this = self.normalize();
        let o = this.offset;
        assert!(o < Self::C8);

        let num_kmers = if this.len == 0 {
            0
        } else {
            (this.len + o).saturating_sub(context - 1)
        };
        // without +o, since we don't need them in the stride.
        let num_kmers_stride = this.len.saturating_sub(context - 1);
        let n = num_kmers_stride.div_ceil(L).next_multiple_of(Self::C8);
        let bytes_per_chunk = n / Self::C8;
        let padding = Self::C8 * L * bytes_per_chunk - num_kmers_stride;

        let offsets: [usize; 8] = from_fn(|l| l * bytes_per_chunk);
        let mut cur = S::ZERO;

        let par_len = if num_kmers == 0 {
            0
        } else {
            n + context + o - 1
        };

        let last_i = par_len.saturating_sub(1) / Self::C32 * Self::C32;
        // Safety check for the `read_slice_32_unchecked`:
        assert!(offsets[7] + (last_i / Self::C8) + 32 <= this.seq.len());

        let it = (0..par_len)
            .map(
                #[inline(always)]
                move |i| {
                    if i % Self::C32 == 0 {
                        if i % Self::C256 == 0 {
                            // Read a u256 for each lane containing the next 128 characters.
                            let data: [u32x8; 8] = from_fn(
                                #[inline(always)]
                                |lane| unsafe {
                                    read_slice_32_unchecked(
                                        this.seq,
                                        offsets[lane] + (i / Self::C8),
                                    )
                                },
                            );
                            *buf = transpose(data);
                        }
                        cur = buf[(i % Self::C256) / Self::C32];
                    }
                    // Extract the last 2 bits of each character.
                    let chars = cur & Self::SIMD_CHAR_MASK;
                    // Shift remaining characters to the right.
                    cur = cur >> Self::SIMD_B;
                    chars
                },
            )
            .advance(o);

        PaddedIt { it, padding }
    }

    #[inline(always)]
    pub fn par_iter_bp_delayed_with_factor(
        self,
        context: usize,
        delay: Delay,
        factor: usize,
    ) -> PaddedIt<impl ChunkIt<(S, S)> + use<'s, B>> {
        self.par_iter_bp_delayed_with_factor_and_buf(context, delay, factor, RecycledVec::take())
    }

    #[inline(always)]
    pub fn par_iter_bp_delayed_with_buf<BUF: DerefMut<Target = Vec<S>>>(
        self,
        context: usize,
        delay: Delay,
        buf: BUF,
    ) -> PaddedIt<impl ChunkIt<(S, S)> + use<'s, B, BUF>> {
        self.par_iter_bp_delayed_with_factor_and_buf(context, delay, 1, buf)
    }

    #[inline(always)]
    pub fn par_iter_bp_delayed_with_factor_and_buf<BUF: DerefMut<Target = Vec<S>>>(
        self,
        context: usize,
        Delay(delay): Delay,
        factor: usize,
        mut buf: BUF,
    ) -> PaddedIt<impl ChunkIt<(S, S)> + use<'s, B, BUF>> {
        #[cfg(target_endian = "big")]
        panic!("Big endian architectures are not supported.");

        assert!(
            delay < usize::MAX / 2,
            "Delay={} should be >=0.",
            delay as isize
        );

        let this = self.normalize();
        let o = this.offset;
        assert!(o < Self::C8);

        let num_kmers = if this.len == 0 {
            0
        } else {
            (this.len + o).saturating_sub(context - 1)
        };
        // without +o, since we don't need them in the stride.
        let num_kmers_stride = this.len.saturating_sub(context - 1);
        let n = num_kmers_stride
            .div_ceil(L)
            .next_multiple_of(factor * Self::C8);
        let bytes_per_chunk = n / Self::C8;
        let padding = Self::C8 * L * bytes_per_chunk - num_kmers_stride;

        let offsets: [usize; 8] = from_fn(|l| l * bytes_per_chunk);
        let mut upcoming = S::ZERO;
        let mut upcoming_d = S::ZERO;

        // Even buf_len is nice to only have the write==buf_len check once.
        // We also make it the next power of 2, for faster modulo operations.
        // delay/16: number of bp in a u32.
        // +8: some 'random' padding
        let buf_len = (delay / Self::C32 + 8).next_power_of_two();
        let buf_mask = buf_len - 1;
        if buf.capacity() < buf_len {
            // This has better codegen than `vec.clear(); vec.resize()`, since the inner `do_reserve_and_handle` of resize is not inlined.
            *buf = vec![S::ZERO; buf_len];
        } else {
            // NOTE: Buf needs to be filled with zeros to guarantee returning 0 values for out-of-bounds characters.
            buf.clear();
            buf.resize(buf_len, S::ZERO);
        }

        let mut write_idx = 0;
        // We compensate for the first delay/16 triggers of the check below that
        // happen before the delay is actually reached.
        let mut read_idx = (buf_len - delay / Self::C32) % buf_len;

        let par_len = if num_kmers == 0 {
            0
        } else {
            n + context + o - 1
        };

        let last_i = par_len.saturating_sub(1) / Self::C32 * Self::C32;
        // Safety check for the `read_slice_32_unchecked`:
        assert!(offsets[7] + (last_i / Self::C8) + 32 <= this.seq.len());

        let it = (0..par_len)
            .map(
                #[inline(always)]
                move |i| {
                    if i % Self::C32 == 0 {
                        if i % Self::C256 == 0 {
                            // Read a u256 for each lane containing the next 128 characters.
                            let data: [u32x8; 8] = from_fn(
                                #[inline(always)]
                                |lane| unsafe {
                                    read_slice_32_unchecked(
                                        this.seq,
                                        offsets[lane] + (i / Self::C8),
                                    )
                                },
                            );
                            unsafe {
                                *TryInto::<&mut [u32x8; 8]>::try_into(
                                    buf.get_unchecked_mut(write_idx..write_idx + 8),
                                )
                                .unwrap_unchecked() = transpose(data);
                            }
                            if i == 0 {
                                // Mask out chars before the offset.
                                let elem = !((1u32 << (B * o)) - 1);
                                let mask = S::splat(elem);
                                unsafe { assert_unchecked(write_idx < buf.len()) };
                                buf[write_idx] &= mask;
                            }
                        }
                        unsafe { assert_unchecked(write_idx < buf.len()) };
                        upcoming = buf[write_idx];
                        write_idx += 1;
                        write_idx &= buf_mask;
                    }
                    if i % Self::C32 == delay % Self::C32 {
                        unsafe { assert_unchecked(read_idx < buf.len()) };
                        upcoming_d = buf[read_idx];
                        read_idx += 1;
                        read_idx &= buf_mask;
                    }
                    // Extract the last 2 bits of each character.
                    let chars = upcoming & Self::SIMD_CHAR_MASK;
                    let chars_d = upcoming_d & Self::SIMD_CHAR_MASK;
                    // Shift remaining characters to the right.
                    upcoming = upcoming >> Self::SIMD_B;
                    upcoming_d = upcoming_d >> Self::SIMD_B;
                    (chars, chars_d)
                },
            )
            .advance(o);

        PaddedIt { it, padding }
    }

    #[inline(always)]
    pub fn par_iter_bp_delayed_2_with_factor(
        self,
        context: usize,
        delay1: Delay,
        delay2: Delay,
        factor: usize,
    ) -> PaddedIt<impl ChunkIt<(S, S, S)> + use<'s, B>> {
        self.par_iter_bp_delayed_2_with_factor_and_buf(
            context,
            delay1,
            delay2,
            factor,
            RecycledVec::take(),
        )
    }

    #[inline(always)]
    pub fn par_iter_bp_delayed_2_with_buf<BUF: DerefMut<Target = Vec<S>>>(
        self,
        context: usize,
        delay1: Delay,
        delay2: Delay,
        buf: BUF,
    ) -> PaddedIt<impl ChunkIt<(S, S, S)> + use<'s, B, BUF>> {
        self.par_iter_bp_delayed_2_with_factor_and_buf(context, delay1, delay2, 1, buf)
    }

    /// When iterating over 2-bit and 1-bit encoded data in parallel,
    /// one must ensure that they have the same stride.
    /// On the larger type, set `factor` as the ratio to the smaller one,
    /// so that the stride in bytes is a multiple of `factor`,
    /// so that the smaller type also has a byte-aligned stride.
    #[inline(always)]
    pub fn par_iter_bp_delayed_2_with_factor_and_buf<BUF: DerefMut<Target = Vec<S>>>(
        self,
        context: usize,
        Delay(delay1): Delay,
        Delay(delay2): Delay,
        factor: usize,
        mut buf: BUF,
    ) -> PaddedIt<impl ChunkIt<(S, S, S)> + use<'s, B, BUF>> {
        #[cfg(target_endian = "big")]
        panic!("Big endian architectures are not supported.");

        let this = self.normalize();
        let o = this.offset;
        assert!(o < Self::C8);
        assert!(delay1 <= delay2, "Delay1 must be at most delay2.");

        let num_kmers = if this.len == 0 {
            0
        } else {
            (this.len + o).saturating_sub(context - 1)
        };
        // without +o, since we don't need them in the stride.
        let num_kmers_stride = this.len.saturating_sub(context - 1);
        let n = num_kmers_stride
            .div_ceil(L)
            .next_multiple_of(factor * Self::C8);
        let bytes_per_chunk = n / Self::C8;
        let padding = Self::C8 * L * bytes_per_chunk - num_kmers_stride;

        let offsets: [usize; 8] = from_fn(|l| l * bytes_per_chunk);
        let mut upcoming = S::ZERO;
        let mut upcoming_d1 = S::ZERO;
        let mut upcoming_d2 = S::ZERO;

        // Even buf_len is nice to only have the write==buf_len check once.
        let buf_len = (delay2 / Self::C32 + 8).next_power_of_two();
        let buf_mask = buf_len - 1;
        if buf.capacity() < buf_len {
            // This has better codegen than `vec.clear(); vec.resize()`, since the inner `do_reserve_and_handle` of resize is not inlined.
            *buf = vec![S::ZERO; buf_len];
        } else {
            // NOTE: Buf needs to be filled with zeros to guarantee returning 0 values for out-of-bounds characters.
            buf.clear();
            buf.resize(buf_len, S::ZERO);
        }

        let mut write_idx = 0;
        // We compensate for the first delay/16 triggers of the check below that
        // happen before the delay is actually reached.
        let mut read_idx1 = (buf_len - delay1 / Self::C32) % buf_len;
        let mut read_idx2 = (buf_len - delay2 / Self::C32) % buf_len;

        let par_len = if num_kmers == 0 {
            0
        } else {
            n + context + o - 1
        };

        let last_i = par_len.saturating_sub(1) / Self::C32 * Self::C32;
        // Safety check for the `read_slice_32_unchecked`:
        assert!(offsets[7] + (last_i / Self::C8) + 32 <= this.seq.len());

        let it = (0..par_len)
            .map(
                #[inline(always)]
                move |i| {
                    if i % Self::C32 == 0 {
                        if i % Self::C256 == 0 {
                            // Read a u256 for each lane containing the next 128 characters.
                            let data: [u32x8; 8] = from_fn(
                                #[inline(always)]
                                |lane| unsafe {
                                    read_slice_32_unchecked(
                                        this.seq,
                                        offsets[lane] + (i / Self::C8),
                                    )
                                },
                            );
                            unsafe {
                                *TryInto::<&mut [u32x8; 8]>::try_into(
                                    buf.get_unchecked_mut(write_idx..write_idx + 8),
                                )
                                .unwrap_unchecked() = transpose(data);
                            }
                            // FIXME DROP THIS?
                            if i == 0 {
                                // Mask out chars before the offset.
                                let elem = !((1u32 << (B * o)) - 1);
                                let mask = S::splat(elem);
                                buf[write_idx] &= mask;
                            }
                        }
                        upcoming = buf[write_idx];
                        write_idx += 1;
                        write_idx &= buf_mask;
                    }
                    if i % Self::C32 == delay1 % Self::C32 {
                        unsafe { assert_unchecked(read_idx1 < buf.len()) };
                        upcoming_d1 = buf[read_idx1];
                        read_idx1 += 1;
                        read_idx1 &= buf_mask;
                    }
                    if i % Self::C32 == delay2 % Self::C32 {
                        unsafe { assert_unchecked(read_idx2 < buf.len()) };
                        upcoming_d2 = buf[read_idx2];
                        read_idx2 += 1;
                        read_idx2 &= buf_mask;
                    }
                    // Extract the last 2 bits of each character.
                    let chars = upcoming & Self::SIMD_CHAR_MASK;
                    let chars_d1 = upcoming_d1 & Self::SIMD_CHAR_MASK;
                    let chars_d2 = upcoming_d2 & Self::SIMD_CHAR_MASK;
                    // Shift remaining characters to the right.
                    upcoming = upcoming >> Self::SIMD_B;
                    upcoming_d1 = upcoming_d1 >> Self::SIMD_B;
                    upcoming_d2 = upcoming_d2 >> Self::SIMD_B;
                    (chars, chars_d1, chars_d2)
                },
            )
            .advance(o);

        PaddedIt { it, padding }
    }
}

impl<const B: usize> PartialEq for PackedSeqBase<'_, B>
where
    Bits<B>: SupportedBits,
{
    /// Compares 29 characters at a time.
    fn eq(&self, other: &Self) -> bool {
        if self.len != other.len {
            return false;
        }
        for i in (0..self.len).step_by(Self::K64) {
            let len = (self.len - i).min(Self::K64);
            let this = self.slice(i..i + len);
            let that = other.slice(i..i + len);
            if this.as_u64() != that.as_u64() {
                return false;
            }
        }
        true
    }
}

impl<const B: usize> Eq for PackedSeqBase<'_, B> where Bits<B>: SupportedBits {}

impl<const B: usize> PartialOrd for PackedSeqBase<'_, B>
where
    Bits<B>: SupportedBits,
{
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl<const B: usize> Ord for PackedSeqBase<'_, B>
where
    Bits<B>: SupportedBits,
{
    /// Compares 29 characters at a time.
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        let min_len = self.len.min(other.len);
        for i in (0..min_len).step_by(Self::K64) {
            let len = (min_len - i).min(Self::K64);
            let this = self.slice(i..i + len);
            let other = other.slice(i..i + len);
            let this_word = this.as_u64();
            let other_word = other.as_u64();
            if this_word != other_word {
                // Unfortunately, bases are packed in little endian order, so the default order is reversed.
                let eq = this_word ^ other_word;
                let t = eq.trailing_zeros() as usize / B * B;
                let mask = (Self::CHAR_MASK) << t;
                return (this_word & mask).cmp(&(other_word & mask));
            }
        }
        self.len.cmp(&other.len)
    }
}

impl<const B: usize> SeqVec for PackedSeqVecBase<B>
where
    Bits<B>: SupportedBits,
{
    type Seq<'s> = PackedSeqBase<'s, B>;

    #[inline(always)]
    fn into_raw(mut self) -> Vec<u8> {
        self.seq.resize(self.len.div_ceil(Self::C8), 0);
        self.seq
    }

    #[inline(always)]
    fn as_slice(&self) -> Self::Seq<'_> {
        PackedSeqBase {
            seq: &self.seq[..self.len.div_ceil(Self::C8) + PADDING],
            offset: 0,
            len: self.len,
        }
    }

    #[inline(always)]
    fn len(&self) -> usize {
        self.len
    }

    #[inline(always)]
    fn is_empty(&self) -> bool {
        self.len == 0
    }

    #[inline(always)]
    fn clear(&mut self) {
        self.seq.clear();
        self.len = 0;
    }

    fn push_seq<'a>(&mut self, seq: PackedSeqBase<'_, B>) -> Range<usize> {
        let start = self.len.next_multiple_of(Self::C8) + seq.offset;
        let end = start + seq.len();

        // Shrink away the padding.
        self.seq.resize(self.len.div_ceil(Self::C8), 0);
        // Extend.
        self.seq.extend(seq.seq);
        self.len = end;
        start..end
    }

    /// For `PackedSeqVec` (2-bit encoding): map ASCII `ACGT` (and `acgt`) to DNA `0132` in that order.
    /// Other characters are silently mapped into `0..4`.
    ///
    /// Uses the BMI2 `pext` instruction when available, based on the
    /// `n_to_bits_pext` method described at
    /// <https://github.com/Daniel-Liu-c0deb0t/cute-nucleotides>.
    ///
    /// For `BitSeqVec` (1-bit encoding): map `ACGTacgt` to `0`, and everything else to `1`.
    fn push_ascii(&mut self, seq: &[u8]) -> Range<usize> {
        match B {
            1 | 2 => {}
            _ => panic!(
                "Can only use ASCII input for 2-bit DNA packing, or 1-bit ambiguous indicators."
            ),
        }

        self.seq
            .resize((self.len + seq.len()).div_ceil(Self::C8) + PADDING, 0);
        let start_aligned = self.len.next_multiple_of(Self::C8);
        let start = self.len;
        let len = seq.len();
        let mut idx = self.len / Self::C8;

        let parse_base = |base| match B {
            1 => char_is_ambiguous(base),
            2 => pack_char_lossy(base),
            _ => unreachable!(),
        };

        let unaligned = core::cmp::min(start_aligned - start, len);
        if unaligned > 0 {
            let mut packed_byte = self.seq[idx];
            for &base in &seq[..unaligned] {
                packed_byte |= parse_base(base) << ((self.len % Self::C8) * B);
                self.len += 1;
            }
            self.seq[idx] = packed_byte;
            idx += 1;
        }

        #[allow(unused)]
        let mut last = unaligned;

        if B == 2 {
            #[cfg(all(target_arch = "x86_64", target_feature = "bmi2"))]
            {
                last = unaligned + (len - unaligned) / 8 * 8;

                for i in (unaligned..last).step_by(8) {
                    let chunk =
                        unsafe { seq.get_unchecked(i..i + 8).try_into().unwrap_unchecked() };
                    let ascii = u64::from_le_bytes(chunk);
                    let packed_bytes =
                        unsafe { std::arch::x86_64::_pext_u64(ascii, 0x0606060606060606) } as u16;
                    unsafe {
                        self.seq
                            .get_unchecked_mut(idx..(idx + 2))
                            .copy_from_slice(&packed_bytes.to_le_bytes())
                    };
                    idx += 2;
                    self.len += 8;
                }
            }

            #[cfg(target_feature = "neon")]
            {
                use core::arch::aarch64::{
                    vandq_u8, vdup_n_u8, vld1q_u8, vpadd_u8, vshlq_u8, vst1_u8,
                };
                use core::mem::transmute;

                last = unaligned + (len - unaligned) / 16 * 16;

                for i in (unaligned..last).step_by(16) {
                    unsafe {
                        let ascii = vld1q_u8(seq.as_ptr().add(i));
                        let masked_bits = vandq_u8(ascii, transmute([6i8; 16]));
                        let (bits_0, bits_1) = transmute(vshlq_u8(
                            masked_bits,
                            transmute([-1i8, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5]),
                        ));
                        let half_packed = vpadd_u8(bits_0, bits_1);
                        let packed = vpadd_u8(half_packed, vdup_n_u8(0));
                        vst1_u8(self.seq.as_mut_ptr().add(idx), packed);
                        idx += Self::C8;
                        self.len += 16;
                    }
                }
            }
        }

        if B == 1 {
            #[cfg(all(target_arch = "x86_64", target_feature = "avx2"))]
            {
                last = len;
                self.len += len - unaligned;

                let mut last_i = unaligned;

                for i in (unaligned..last).step_by(32) {
                    use std::mem::transmute as t;

                    use wide::CmpEq;
                    // Wide doesn't have u8x32, so this is messy here...
                    type S = wide::i8x32;
                    let chars: S = unsafe { t(read_slice_32(seq, i)) };
                    let upper_mask = !(b'a' - b'A');
                    // make everything upper case
                    let chars = chars & S::splat(upper_mask as i8);
                    let lossy_encoded = chars & S::splat(6);
                    let table = unsafe { S::from(t::<_, S>(*b"AxCxTxGxxxxxxxxxAxCxTxGxxxxxxxxx")) };
                    let lookup: S = unsafe {
                        t(std::arch::x86_64::_mm256_shuffle_epi8(
                            t(table),
                            t(lossy_encoded),
                        ))
                    };
                    let packed_bytes = !(chars.cmp_eq(lookup).move_mask() as u32);

                    last_i = i;
                    unsafe {
                        self.seq
                            .get_unchecked_mut(idx..(idx + 4))
                            .copy_from_slice(&packed_bytes.to_le_bytes())
                    };
                    idx += 4;
                }

                // Fix up trailing bytes.
                if unaligned < last {
                    idx -= 4;
                    let mut val = unsafe {
                        u32::from_le_bytes(
                            self.seq
                                .get_unchecked(idx..(idx + 4))
                                .try_into()
                                .unwrap_unchecked(),
                        )
                    };
                    // keep only the `last - last_i` low bits.
                    let keep = last - last_i;
                    val <<= 32 - keep;
                    val >>= 32 - keep;
                    unsafe {
                        self.seq
                            .get_unchecked_mut(idx..(idx + 4))
                            .copy_from_slice(&val.to_le_bytes())
                    };
                    idx += keep.div_ceil(8);
                }
            }

            #[cfg(target_feature = "neon")]
            {
                use core::arch::aarch64::*;
                use core::mem::transmute;

                last = unaligned + (len - unaligned) / 64 * 64;

                for i in (unaligned..last).step_by(64) {
                    unsafe {
                        let ptr = seq.as_ptr().add(i);
                        let chars = vld4q_u8(ptr);

                        let upper_mask = vdupq_n_u8(!(b'a' - b'A'));
                        let chars = neon::map_8x16x4(chars, |v| vandq_u8(v, upper_mask));

                        let two_bits_mask = vdupq_n_u8(6);
                        let lossy_encoded = neon::map_8x16x4(chars, |v| vandq_u8(v, two_bits_mask));

                        let table = transmute(*b"AxCxTxGxxxxxxxxx");
                        let lookup = neon::map_8x16x4(lossy_encoded, |v| vqtbl1q_u8(table, v));

                        let mask = neon::map_two_8x16x4(chars, lookup, |v1, v2| vceqq_u8(v1, v2));
                        let packed_bytes = !neon::movemask_64(mask);

                        self.seq[idx..(idx + 8)].copy_from_slice(&packed_bytes.to_le_bytes());
                        idx += 8;
                        self.len += 64;
                    }
                }
            }
        }

        let mut packed_byte = 0;
        for &base in &seq[last..] {
            packed_byte |= parse_base(base) << ((self.len % Self::C8) * B);
            self.len += 1;
            if self.len % Self::C8 == 0 {
                self.seq[idx] = packed_byte;
                idx += 1;
                packed_byte = 0;
            }
        }
        if self.len % Self::C8 != 0 && last < len {
            self.seq[idx] = packed_byte;
            idx += 1;
        }
        assert_eq!(idx + PADDING, self.seq.len());
        start..start + len
    }

    #[cfg(feature = "rand")]
    fn random(n: usize) -> Self {
        use rand::{RngCore, SeedableRng};

        let byte_len = n.div_ceil(Self::C8);
        let mut seq = vec![0; byte_len + PADDING];
        rand::rngs::SmallRng::from_os_rng().fill_bytes(&mut seq[..byte_len]);
        // Ensure that the last byte is padded with zeros.
        if n % Self::C8 != 0 {
            seq[byte_len - 1] &= (1 << (B * (n % Self::C8))) - 1;
        }

        Self { seq, len: n }
    }
}

impl<const B: usize> PackedSeqVecBase<B>
where
    Bits<B>: SupportedBits,
{
    /// Creates a `SeqVec` from a vector of packed bytes and a length in bp.
    ///
    /// The vector should have at least 48 bytes of padding after `len`.
    /// Otherwise, the vector will be resized to be padded with zeros.
    pub fn from_raw_parts(mut seq: Vec<u8>, len: usize) -> Self {
        assert!(len <= seq.len() * Self::C8);
        seq.resize(len.div_ceil(Self::C8) + PADDING, 0);
        Self { seq, len }
    }
}

impl PackedSeqVecBase<1> {
    pub fn with_len(n: usize) -> Self {
        Self {
            seq: vec![0; n.div_ceil(Self::C8) + PADDING],
            len: n,
        }
    }

    #[cfg(feature = "rand")]
    pub fn random(len: usize, n_frac: f32) -> Self {
        let byte_len = len.div_ceil(Self::C8);
        let mut seq = vec![0; byte_len + PADDING];

        assert!(
            (0.0..=0.3).contains(&n_frac),
            "n_frac={} should be in [0, 0.3]",
            n_frac
        );

        for _ in 0..(len as f32 * n_frac) as usize {
            let idx = rand::random::<u64>() as usize % len;
            let byte = idx / Self::C8;
            let offset = idx % Self::C8;
            seq[byte] |= 1 << offset;
        }

        Self { seq, len }
    }
}

impl<'s> PackedSeqBase<'s, 1> {
    /// An iterator indicating for each kmer whether it contains ambiguous bases.
    ///
    /// Returns n-(k-1) elements.
    #[inline(always)]
    pub fn iter_kmer_ambiguity(self, k: usize) -> impl ExactSizeIterator<Item = bool> + use<'s> {
        let this = self.normalize();
        assert!(k > 0);
        assert!(k <= Self::K64);
        (this.offset..this.offset + this.len.saturating_sub(k - 1))
            .map(move |i| self.read_kmer(k, i) != 0)
    }

    /// A parallel iterator indicating for each kmer whether it contains ambiguous bases.
    ///
    /// First element is the 'kmer' consisting only of the first character of each chunk.
    ///
    /// `k`: length of windows to check
    /// `context`: number of overlapping iterations +1. To determine stride of each lane.
    /// `skip`: Set to `context-1` to skip the iterations added by the context.
    #[inline(always)]
    pub fn par_iter_kmer_ambiguity(
        self,
        k: usize,
        context: usize,
        skip: usize,
    ) -> PaddedIt<impl ChunkIt<S> + use<'s>> {
        #[cfg(target_endian = "big")]
        panic!("Big endian architectures are not supported.");

        assert!(k > 0, "par_iter_kmers requires k>0, but k={k}");
        assert!(k <= 96, "par_iter_kmers requires k<=96, but k={k}");

        let this = self.normalize();
        let o = this.offset;
        assert!(o < Self::C8);

        let delay = k - 1;

        let it = self.par_iter_bp_delayed(context, Delay(delay));

        let mut cnt = u32x8::ZERO;

        it.map(
            #[inline(always)]
            move |(a, r)| {
                cnt += a;
                let out = cnt.cmp_gt(S::ZERO);
                cnt -= r;
                out
            },
        )
        .advance(skip)
    }

    #[inline(always)]
    pub fn par_iter_kmer_ambiguity_with_buf(
        self,
        k: usize,
        context: usize,
        skip: usize,
        buf: &'s mut Vec<S>,
    ) -> PaddedIt<impl ChunkIt<S> + use<'s>> {
        #[cfg(target_endian = "big")]
        panic!("Big endian architectures are not supported.");

        assert!(k > 0, "par_iter_kmers requires k>0, but k={k}");
        assert!(k <= 96, "par_iter_kmers requires k<=96, but k={k}");

        let this = self.normalize();
        let o = this.offset;
        assert!(o < Self::C8);

        let delay = k - 1;

        let it = self.par_iter_bp_delayed_with_buf(context, Delay(delay), buf);

        let mut cnt = u32x8::ZERO;

        it.map(
            #[inline(always)]
            move |(a, r)| {
                cnt += a;
                let out = cnt.cmp_gt(S::ZERO);
                cnt -= r;
                out
            },
        )
        .advance(skip)
    }
}

#[cfg(target_feature = "neon")]
mod neon {
    use core::arch::aarch64::*;

    #[inline(always)]
    pub fn movemask_64(v: uint8x16x4_t) -> u64 {
        // https://stackoverflow.com/questions/74722950/convert-vector-compare-mask-into-bit-mask-in-aarch64-simd-or-arm-neon/74748402#74748402
        unsafe {
            let acc = vsriq_n_u8(vsriq_n_u8(v.3, v.2, 1), vsriq_n_u8(v.1, v.0, 1), 2);
            vget_lane_u64(
                vreinterpret_u64_u8(vshrn_n_u16(
                    vreinterpretq_u16_u8(vsriq_n_u8(acc, acc, 4)),
                    4,
                )),
                0,
            )
        }
    }

    #[inline(always)]
    pub fn map_8x16x4<F>(v: uint8x16x4_t, mut f: F) -> uint8x16x4_t
    where
        F: FnMut(uint8x16_t) -> uint8x16_t,
    {
        uint8x16x4_t(f(v.0), f(v.1), f(v.2), f(v.3))
    }

    #[inline(always)]
    pub fn map_two_8x16x4<F>(v1: uint8x16x4_t, v2: uint8x16x4_t, mut f: F) -> uint8x16x4_t
    where
        F: FnMut(uint8x16_t, uint8x16_t) -> uint8x16_t,
    {
        uint8x16x4_t(f(v1.0, v2.0), f(v1.1, v2.1), f(v1.2, v2.2), f(v1.3, v2.3))
    }
}
