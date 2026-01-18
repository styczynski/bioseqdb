use crate::{intrinsics::transpose, packed_seq::read_slice_32, padded_it::ChunkIt};

use super::*;

/// A `Vec<u8>` representing an ASCII-encoded DNA sequence of `ACGTacgt`.
///
/// Other characters will be mapped into `[0, 4)` via `(c>>1)&3`, or may cause panics.
#[derive(Clone, Debug, Default, MemSize, MemDbg)]
#[cfg_attr(feature = "pyo3", pyo3::pyclass)]
#[cfg_attr(feature = "epserde", derive(epserde::Epserde))]
pub struct AsciiSeqVec {
    pub seq: Vec<u8>,
}

/// A `&[u8]` representing an ASCII-encoded DNA sequence of `ACGTacgt`.
///
/// Other characters will be mapped into `[0, 4)` via `(c>>1)&3`, or may cause panics.
#[derive(Copy, Clone, Debug, MemSize, MemDbg, PartialEq, Eq, PartialOrd, Ord)]
pub struct AsciiSeq<'s>(pub &'s [u8]);

/// Maps ASCII to `[0, 4)` on the fly.
/// Prefer first packing into a `PackedSeqVec` for storage.
impl<'s> Seq<'s> for AsciiSeq<'s> {
    /// Each input byte stores a single character.
    const BASES_PER_BYTE: usize = 1;
    /// But each output bp only takes 2 bits!
    const BITS_PER_CHAR: usize = 2;
    type SeqVec = AsciiSeqVec;

    #[inline(always)]
    fn len(&self) -> usize {
        self.0.len()
    }

    #[inline(always)]
    fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    #[inline(always)]
    fn get(&self, index: usize) -> u8 {
        pack_char(self.0[index])
    }

    #[inline(always)]
    fn get_ascii(&self, index: usize) -> u8 {
        self.0[index]
    }

    #[inline(always)]
    fn as_u64(&self) -> u64 {
        let len = self.len();
        assert!(len <= u64::BITS as usize / 2);

        let mut val = 0u64;

        #[cfg(all(target_arch = "x86_64", target_feature = "bmi2"))]
        {
            for i in (0..len).step_by(8) {
                let packed_bytes = if i + 8 <= self.len() {
                    let chunk: &[u8; 8] = &self.0[i..i + 8].try_into().unwrap();
                    let ascii = u64::from_ne_bytes(*chunk);
                    unsafe { std::arch::x86_64::_pext_u64(ascii, 0x0606060606060606) }
                } else {
                    let mut chunk: [u8; 8] = [0; 8];
                    // Copy only part of the slice to avoid out-of-bounds indexing.
                    chunk[..self.len() - i].copy_from_slice(self.0[i..].try_into().unwrap());
                    let ascii = u64::from_ne_bytes(chunk);
                    unsafe { std::arch::x86_64::_pext_u64(ascii, 0x0606060606060606) }
                };
                val |= packed_bytes << (i * 2);
            }
        }

        #[cfg(target_feature = "neon")]
        {
            use core::arch::aarch64::{vandq_u8, vdup_n_u8, vld1q_u8, vpadd_u8, vshlq_u8};
            use core::mem::transmute;

            for i in (0..len).step_by(16) {
                let packed_bytes: u64 = if i + 16 <= self.len() {
                    unsafe {
                        let ascii = vld1q_u8(self.0.as_ptr().add(i));
                        let masked_bits = vandq_u8(ascii, transmute([6i8; 16]));
                        let (bits_0, bits_1) = transmute(vshlq_u8(
                            masked_bits,
                            transmute([-1i8, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5]),
                        ));
                        let half_packed = vpadd_u8(bits_0, bits_1);
                        let packed = vpadd_u8(half_packed, vdup_n_u8(0));
                        transmute(packed)
                    }
                } else {
                    let mut chunk: [u8; 16] = [0; 16];
                    // Copy only part of the slice to avoid out-of-bounds indexing.
                    chunk[..self.len() - i].copy_from_slice(self.0[i..].try_into().unwrap());
                    unsafe {
                        let ascii = vld1q_u8(chunk.as_ptr());
                        let masked_bits = vandq_u8(ascii, transmute([6i8; 16]));
                        let (bits_0, bits_1) = transmute(vshlq_u8(
                            masked_bits,
                            transmute([-1i8, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5]),
                        ));
                        let half_packed = vpadd_u8(bits_0, bits_1);
                        let packed = vpadd_u8(half_packed, vdup_n_u8(0));
                        transmute(packed)
                    }
                };
                val |= packed_bytes << (i * 2);
            }
        }

        #[cfg(not(any(
            all(target_arch = "x86_64", target_feature = "bmi2"),
            target_feature = "neon"
        )))]
        {
            for (i, &base) in self.0.iter().enumerate() {
                val |= (pack_char(base) as u64) << (i * 2);
            }
        }

        val
    }

    // TODO: Dedup against as_u64.
    #[inline(always)]
    fn as_u128(&self) -> u128 {
        let len = self.len();
        assert!(len <= u128::BITS as usize / 2);

        let mut val = 0u128;

        #[cfg(all(target_arch = "x86_64", target_feature = "bmi2"))]
        {
            for i in (0..len).step_by(8) {
                let packed_bytes = if i + 8 <= self.len() {
                    let chunk: &[u8; 8] = &self.0[i..i + 8].try_into().unwrap();
                    let ascii = u64::from_ne_bytes(*chunk);
                    unsafe { std::arch::x86_64::_pext_u64(ascii, 0x0606060606060606) }
                } else {
                    let mut chunk: [u8; 8] = [0; 8];
                    // Copy only part of the slice to avoid out-of-bounds indexing.
                    chunk[..self.len() - i].copy_from_slice(self.0[i..].try_into().unwrap());
                    let ascii = u64::from_ne_bytes(chunk);
                    unsafe { std::arch::x86_64::_pext_u64(ascii, 0x0606060606060606) }
                };
                val |= (packed_bytes as u128) << (i * 2);
            }
        }

        #[cfg(target_feature = "neon")]
        {
            use core::arch::aarch64::{vandq_u8, vdup_n_u8, vld1q_u8, vpadd_u8, vshlq_u8};
            use core::mem::transmute;

            for i in (0..len).step_by(16) {
                let packed_bytes: u64 = if i + 16 <= self.len() {
                    unsafe {
                        let ascii = vld1q_u8(self.0.as_ptr().add(i));
                        let masked_bits = vandq_u8(ascii, transmute([6i8; 16]));
                        let (bits_0, bits_1) = transmute(vshlq_u8(
                            masked_bits,
                            transmute([-1i8, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5]),
                        ));
                        let half_packed = vpadd_u8(bits_0, bits_1);
                        let packed = vpadd_u8(half_packed, vdup_n_u8(0));
                        transmute(packed)
                    }
                } else {
                    let mut chunk: [u8; 16] = [0; 16];
                    // Copy only part of the slice to avoid out-of-bounds indexing.
                    chunk[..self.len() - i].copy_from_slice(self.0[i..].try_into().unwrap());
                    unsafe {
                        let ascii = vld1q_u8(chunk.as_ptr());
                        let masked_bits = vandq_u8(ascii, transmute([6i8; 16]));
                        let (bits_0, bits_1) = transmute(vshlq_u8(
                            masked_bits,
                            transmute([-1i8, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5, -1, 1, 3, 5]),
                        ));
                        let half_packed = vpadd_u8(bits_0, bits_1);
                        let packed = vpadd_u8(half_packed, vdup_n_u8(0));
                        transmute(packed)
                    }
                };
                val |= (packed_bytes as u128) << (i * 2);
            }
        }

        #[cfg(not(any(
            all(target_arch = "x86_64", target_feature = "bmi2"),
            target_feature = "neon"
        )))]
        {
            for (i, &base) in self.0.iter().enumerate() {
                val |= (pack_char(base) as u128) << (i * 2);
            }
        }

        val
    }

    #[inline(always)]
    fn revcomp_as_u64(&self) -> u64 {
        packed_seq::revcomp_u64(self.as_u64(), self.len())
    }

    #[inline(always)]
    fn revcomp_as_u128(&self) -> u128 {
        packed_seq::revcomp_u128(self.as_u128(), self.len())
    }

    /// Convert to an owned version.
    #[inline(always)]
    fn to_vec(&self) -> AsciiSeqVec {
        AsciiSeqVec {
            seq: self.0.to_vec(),
        }
    }

    #[inline(always)]
    fn to_revcomp(&self) -> AsciiSeqVec {
        AsciiSeqVec {
            seq: self
                .0
                .iter()
                .rev()
                .copied()
                .map(packed_seq::complement_char)
                .collect(),
        }
    }

    #[inline(always)]
    fn slice(&self, range: Range<usize>) -> Self {
        Self(&self.0[range])
    }

    /// Iterate the basepairs in the sequence, assuming values in `0..4`.
    ///
    /// NOTE: This is only efficient on x86_64 with `BMI2` support for `pext`.
    #[inline(always)]
    fn iter_bp(self) -> impl ExactSizeIterator<Item = u8> {
        #[cfg(all(target_arch = "x86_64", target_feature = "bmi2"))]
        {
            let mut cache = 0;
            (0..self.len()).map(
                #[inline(always)]
                move |i| {
                    if i % 8 == 0 {
                        if i + 8 <= self.len() {
                            let chunk: &[u8; 8] = &self.0[i..i + 8].try_into().unwrap();
                            let ascii = u64::from_ne_bytes(*chunk);
                            cache = ascii >> 1;
                        } else {
                            let mut chunk: [u8; 8] = [0; 8];
                            // Copy only part of the slice to avoid out-of-bounds indexing.
                            chunk[..self.len() - i]
                                .copy_from_slice(self.0[i..].try_into().unwrap());
                            let ascii = u64::from_ne_bytes(chunk);
                            cache = ascii >> 1;
                        }
                    }
                    let base = cache & 0x03;
                    cache >>= 8;
                    base as u8
                },
            )
        }

        #[cfg(not(all(target_arch = "x86_64", target_feature = "bmi2")))]
        self.0.iter().copied().map(pack_char)
    }

    /// Iterate the basepairs in the sequence in 8 parallel streams, assuming values in `0..4`.
    #[inline(always)]
    fn par_iter_bp(self, context: usize) -> PaddedIt<impl ChunkIt<S>> {
        let num_kmers = self.len().saturating_sub(context - 1);
        let n = num_kmers.div_ceil(L);
        let padding = L * n - num_kmers;

        let offsets: [usize; 8] = from_fn(|l| l * n);
        let mut cur = S::ZERO;

        // Boxed, so it doesn't consume precious registers.
        // Without this, cur is not always inlined into a register.
        let mut buf = Box::new([S::ZERO; 8]);

        let par_len = if num_kmers == 0 { 0 } else { n + context - 1 };
        let it = (0..par_len).map(
            #[inline(always)]
            move |i| {
                if i % 4 == 0 {
                    if i % 32 == 0 {
                        // Read a u256 for each lane containing the next 32 characters.
                        let data: [u32x8; 8] = from_fn(
                            #[inline(always)]
                            |lane| read_slice_32(self.0, offsets[lane] + i),
                        );
                        *buf = transpose(data);
                        for x in buf.iter_mut() {
                            *x = *x >> 1;
                        }
                    }
                    cur = buf[(i % 32) / 4];
                }
                // Extract the last 2 bits of each character.
                let chars = cur & S::splat(0x03);
                // Shift remaining characters to the right.
                cur = cur >> S::splat(8);
                chars
            },
        );

        PaddedIt { it, padding }
    }

    #[inline(always)]
    fn par_iter_bp_delayed(
        self,
        context: usize,
        Delay(delay): Delay,
    ) -> PaddedIt<impl ChunkIt<(S, S)>> {
        assert!(
            delay < usize::MAX / 2,
            "Delay={} should be >=0.",
            delay as isize
        );

        let num_kmers = self.len().saturating_sub(context - 1);
        let n = num_kmers.div_ceil(L);
        let padding = L * n - num_kmers;

        let offsets: [usize; 8] = from_fn(|l| l * n);
        let mut upcoming = S::ZERO;
        let mut upcoming_d = S::ZERO;

        // Even buf_len is nice to only have the write==buf_len check once.
        // We also make it the next power of 2, for faster modulo operations.
        // delay/4: number of bp in a u32.
        let buf_len = (delay / 4 + 8).next_power_of_two();
        let buf_mask = buf_len - 1;
        let mut buf = vec![S::ZERO; buf_len];
        let mut write_idx = 0;
        // We compensate for the first delay/16 triggers of the check below that
        // happen before the delay is actually reached.
        let mut read_idx = (buf_len - delay / 4) % buf_len;

        let par_len = if num_kmers == 0 { 0 } else { n + context - 1 };
        let it = (0..par_len).map(
            #[inline(always)]
            move |i| {
                if i % 4 == 0 {
                    if i % 32 == 0 {
                        // Read a u256 for each lane containing the next 32 characters.
                        let data: [u32x8; 8] = from_fn(
                            #[inline(always)]
                            |lane| read_slice_32(self.0, offsets[lane] + i),
                        );
                        unsafe {
                            let mut_array: &mut [u32x8; 8] = buf
                                .get_unchecked_mut(write_idx..write_idx + 8)
                                .try_into()
                                .unwrap_unchecked();
                            *mut_array = transpose(data);
                            for x in mut_array {
                                *x = *x >> 1;
                            }
                        }
                    }
                    upcoming = buf[write_idx];
                    write_idx += 1;
                    write_idx &= buf_mask;
                }
                if i % 4 == delay % 4 {
                    unsafe { assert_unchecked(read_idx < buf.len()) };
                    upcoming_d = buf[read_idx];
                    read_idx += 1;
                    read_idx &= buf_mask;
                }
                // Extract the last 2 bits of each character.
                let chars = upcoming & S::splat(0x03);
                let chars_d = upcoming_d & S::splat(0x03);
                // Shift remaining characters to the right.
                upcoming = upcoming >> S::splat(8);
                upcoming_d = upcoming_d >> S::splat(8);
                (chars, chars_d)
            },
        );

        PaddedIt { it, padding }
    }

    #[inline(always)]
    fn par_iter_bp_delayed_2(
        self,
        context: usize,
        Delay(delay1): Delay,
        Delay(delay2): Delay,
    ) -> PaddedIt<impl ChunkIt<(S, S, S)>> {
        assert!(delay1 <= delay2, "Delay1 must be at most delay2.");

        let num_kmers = self.len().saturating_sub(context - 1);
        let n = num_kmers.div_ceil(L);
        let padding = L * n - num_kmers;

        let offsets: [usize; 8] = from_fn(|l| l * n);

        let mut upcoming = S::ZERO;
        let mut upcoming_d1 = S::ZERO;
        let mut upcoming_d2 = S::ZERO;

        // Even buf_len is nice to only have the write==buf_len check once.
        // We also make it the next power of 2, for faster modulo operations.
        // delay/4: number of bp in a u32.
        let buf_len = (delay2 / 4 + 8).next_power_of_two();
        let buf_mask = buf_len - 1;
        let mut buf = vec![S::ZERO; buf_len];
        let mut write_idx = 0;
        // We compensate for the first delay/16 triggers of the check below that
        // happen before the delay is actually reached.
        let mut read_idx1 = (buf_len - delay1 / 4) % buf_len;
        let mut read_idx2 = (buf_len - delay2 / 4) % buf_len;

        let par_len = if num_kmers == 0 { 0 } else { n + context - 1 };
        let it = (0..par_len).map(
            #[inline(always)]
            move |i| {
                if i % 4 == 0 {
                    if i % 32 == 0 {
                        // Read a u256 for each lane containing the next 32 characters.
                        let data: [u32x8; 8] = from_fn(
                            #[inline(always)]
                            |lane| read_slice_32(self.0, offsets[lane] + i),
                        );
                        unsafe {
                            let mut_array: &mut [u32x8; 8] = buf
                                .get_unchecked_mut(write_idx..write_idx + 8)
                                .try_into()
                                .unwrap_unchecked();
                            *mut_array = transpose(data);
                            for x in mut_array {
                                *x = *x >> 1;
                            }
                        }
                    }
                    upcoming = buf[write_idx];
                    write_idx += 1;
                    write_idx &= buf_mask;
                }
                if i % 4 == delay1 % 4 {
                    unsafe { assert_unchecked(read_idx1 < buf.len()) };
                    upcoming_d1 = buf[read_idx1];
                    read_idx1 += 1;
                    read_idx1 &= buf_mask;
                }
                if i % 4 == delay2 % 4 {
                    unsafe { assert_unchecked(read_idx2 < buf.len()) };
                    upcoming_d2 = buf[read_idx2];
                    read_idx2 += 1;
                    read_idx2 &= buf_mask;
                }
                // Extract the last 2 bits of each character.
                let chars = upcoming & S::splat(0x03);
                let chars_d1 = upcoming_d1 & S::splat(0x03);
                let chars_d2 = upcoming_d2 & S::splat(0x03);
                // Shift remaining characters to the right.
                upcoming = upcoming >> S::splat(8);
                upcoming_d1 = upcoming_d1 >> S::splat(8);
                upcoming_d2 = upcoming_d2 >> S::splat(8);
                (chars, chars_d1, chars_d2)
            },
        );

        PaddedIt { it, padding }
    }

    // TODO: This is not very optimized.
    fn cmp_lcp(&self, other: &Self) -> (std::cmp::Ordering, usize) {
        for i in 0..self.len().min(other.len()) {
            if self.0[i] != other.0[i] {
                return (self.0[i].cmp(&other.0[i]), i);
            }
        }
        (self.len().cmp(&other.len()), self.len().min(other.len()))
    }
}

impl AsciiSeqVec {
    #[inline(always)]
    pub const fn from_vec(seq: Vec<u8>) -> Self {
        Self { seq }
    }
}

impl SeqVec for AsciiSeqVec {
    type Seq<'s> = AsciiSeq<'s>;

    /// Get the underlying ASCII text.
    #[inline(always)]
    fn into_raw(self) -> Vec<u8> {
        self.seq
    }

    #[inline(always)]
    fn as_slice(&self) -> Self::Seq<'_> {
        AsciiSeq(self.seq.as_slice())
    }

    #[inline(always)]
    fn len(&self) -> usize {
        self.seq.len()
    }

    #[inline(always)]
    fn is_empty(&self) -> bool {
        self.seq.is_empty()
    }

    #[inline(always)]
    fn clear(&mut self) {
        self.seq.clear()
    }

    #[inline(always)]
    fn push_seq(&mut self, seq: AsciiSeq) -> Range<usize> {
        let start = self.seq.len();
        let end = start + seq.len();
        let range = start..end;
        self.seq.extend(seq.0);
        range
    }

    #[inline(always)]
    fn push_ascii(&mut self, seq: &[u8]) -> Range<usize> {
        self.push_seq(AsciiSeq(seq))
    }

    #[cfg(feature = "rand")]
    fn random(n: usize) -> Self {
        use rand::{RngCore, SeedableRng};

        let mut seq = vec![0; n];
        rand::rngs::SmallRng::from_os_rng().fill_bytes(&mut seq);
        Self {
            seq: seq.into_iter().map(|b| b"ACGT"[b as usize % 4]).collect(),
        }
    }
}
