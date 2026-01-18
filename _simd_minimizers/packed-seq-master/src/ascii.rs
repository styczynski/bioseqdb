use crate::{intrinsics::transpose, packed_seq::read_slice_32, padded_it::ChunkIt};

use super::*;

/// Maps ASCII to `[0, 4)` on the fly.
/// Prefer first packing into a `PackedSeqVec` for storage.
impl Seq<'_> for &[u8] {
    const BASES_PER_BYTE: usize = 1;
    const BITS_PER_CHAR: usize = 8;
    type SeqVec = Vec<u8>;

    #[inline(always)]
    fn len(&self) -> usize {
        <[u8]>::len(self)
    }

    #[inline(always)]
    fn is_empty(&self) -> bool {
        <[u8]>::is_empty(self)
    }

    #[inline(always)]
    fn get(&self, index: usize) -> u8 {
        self[index]
    }

    #[inline(always)]
    fn get_ascii(&self, index: usize) -> u8 {
        self[index]
    }

    #[inline(always)]
    fn as_u64(&self) -> u64 {
        assert!(self.len() <= u64::BITS as usize / 8);
        let mask = u64::MAX >> (64 - 8 * self.len());
        unsafe { (self.as_ptr() as *const u64).read_unaligned() & mask }
    }

    #[inline(always)]
    fn revcomp_as_u64(&self) -> u64 {
        unimplemented!(
            "Reverse complement is only defined for DNA sequences, use `AsciiSeq` or `PackedSeq` instead."
        )
    }

    #[inline(always)]
    fn as_u128(&self) -> u128 {
        assert!(self.len() <= u128::BITS as usize / 8);
        let mask = u128::MAX >> (128 - 8 * self.len());
        unsafe { (self.as_ptr() as *const u128).read_unaligned() & mask }
    }

    #[inline(always)]
    fn revcomp_as_u128(&self) -> u128 {
        unimplemented!(
            "Reverse complement is only defined for DNA sequences, use `AsciiSeq` or `PackedSeq` instead."
        )
    }

    /// Convert to an owned version.
    #[inline(always)]
    fn to_vec(&self) -> Vec<u8> {
        <[u8]>::to_vec(self)
    }

    #[inline(always)]
    fn to_revcomp(&self) -> Vec<u8> {
        unimplemented!(
            "Reverse complement is only defined for DNA sequences, use `AsciiSeq` or `PackedSeq` instead."
        )
    }

    #[inline(always)]
    fn slice(&self, range: Range<usize>) -> Self {
        &self[range]
    }

    /// Iter the ASCII characters.
    #[inline(always)]
    fn iter_bp(self) -> impl ExactSizeIterator<Item = u8> {
        self.iter().copied()
    }

    /// Iter the ASCII characters in parallel.
    #[inline(always)]
    fn par_iter_bp(self, context: usize) -> PaddedIt<impl ChunkIt<u32x8>> {
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
                            |lane| read_slice_32(self, offsets[lane] + i),
                        );
                        *buf = transpose(data);
                    }
                    cur = buf[(i % 32) / 4];
                }
                // Extract the last 2 bits of each character.
                let chars = cur & S::splat(0xff);
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
    ) -> PaddedIt<impl ChunkIt<(u32x8, u32x8)>> {
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
                            |lane| read_slice_32(self, offsets[lane] + i),
                        );
                        unsafe {
                            let mut_array: &mut [u32x8; 8] = buf
                                .get_unchecked_mut(write_idx..write_idx + 8)
                                .try_into()
                                .unwrap_unchecked();
                            *mut_array = transpose(data);
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
                let chars = upcoming & S::splat(0xff);
                let chars_d = upcoming_d & S::splat(0xff);
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
    ) -> PaddedIt<impl ChunkIt<(u32x8, u32x8, u32x8)>> {
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
                            |lane| read_slice_32(self, offsets[lane] + i),
                        );
                        unsafe {
                            let mut_array: &mut [u32x8; 8] = buf
                                .get_unchecked_mut(write_idx..write_idx + 8)
                                .try_into()
                                .unwrap_unchecked();
                            *mut_array = transpose(data);
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
                let chars = upcoming & S::splat(0xff);
                let chars_d1 = upcoming_d1 & S::splat(0xff);
                let chars_d2 = upcoming_d2 & S::splat(0xff);
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
            if self[i] != other[i] {
                return (self[i].cmp(&other[i]), i);
            }
        }
        (self.len().cmp(&other.len()), self.len().min(other.len()))
    }
}

impl SeqVec for Vec<u8> {
    type Seq<'s> = &'s [u8];

    /// Get the underlying ASCII text.
    #[inline(always)]
    fn into_raw(self) -> Vec<u8> {
        self
    }

    #[inline(always)]
    fn as_slice(&self) -> Self::Seq<'_> {
        self.as_slice()
    }

    #[inline(always)]
    fn len(&self) -> usize {
        self.len()
    }

    #[inline(always)]
    fn is_empty(&self) -> bool {
        self.is_empty()
    }

    #[inline(always)]
    fn clear(&mut self) {
        self.clear()
    }

    #[inline(always)]
    fn push_seq(&mut self, seq: &[u8]) -> Range<usize> {
        let start = self.len();
        let end = start + seq.len();
        let range = start..end;
        self.extend(seq);
        range
    }

    #[inline(always)]
    fn push_ascii(&mut self, seq: &[u8]) -> Range<usize> {
        self.push_seq(seq)
    }

    #[cfg(feature = "rand")]
    fn random(n: usize) -> Self {
        use rand::{RngCore, SeedableRng};

        let mut seq = vec![0; n];
        rand::rngs::SmallRng::from_os_rng().fill_bytes(&mut seq);
        seq
    }
}
