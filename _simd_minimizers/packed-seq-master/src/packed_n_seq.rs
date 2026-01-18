use wide::{CmpLt, i8x32};

use crate::packed_seq::read_slice_32;

use super::*;

#[derive(Clone, Copy, Debug, MemSize, MemDbg)]
#[cfg_attr(feature = "pyo3", pyo3::pyclass)]
#[cfg_attr(feature = "epserde", derive(epserde::Epserde))]
pub struct PackedNSeq<'s> {
    pub seq: PackedSeq<'s>,
    pub ambiguous: BitSeq<'s>,
}

#[derive(Clone, Debug, MemSize, MemDbg, Default)]
#[cfg_attr(feature = "pyo3", pyo3::pyclass)]
#[cfg_attr(feature = "epserde", derive(epserde::Epserde))]
pub struct PackedNSeqVec {
    pub seq: PackedSeqVec,
    pub ambiguous: BitSeqVec,
}

/// Implement a subset of `Seq` for `PackedNSeq`.
impl<'s> PackedNSeq<'s> {
    pub fn to_revcomp(&self) -> PackedNSeqVec {
        PackedNSeqVec {
            seq: self.seq.to_revcomp(),
            ambiguous: self.ambiguous.to_revcomp(),
        }
    }

    pub fn slice(&self, range: Range<usize>) -> PackedNSeq<'s> {
        PackedNSeq {
            seq: self.seq.slice(range.clone()),
            ambiguous: self.ambiguous.slice(range),
        }
    }
}

/// Implement a subset of `SeqVec` for `PackedNSeqVec`.
impl PackedNSeqVec {
    pub fn clear(&mut self) {
        self.seq.clear();
        self.ambiguous.clear();
    }

    #[cfg(feature = "rand")]
    pub fn random(len: usize, n_frac: f32) -> Self {
        let seq = PackedSeqVec::random(len);
        let ambiguous = BitSeqVec::random(len, n_frac);
        Self { seq, ambiguous }
    }

    pub fn as_slice(&self) -> PackedNSeq<'_> {
        PackedNSeq {
            seq: self.seq.as_slice(),
            ambiguous: self.ambiguous.as_slice(),
        }
    }

    pub fn slice(&self, range: Range<usize>) -> PackedNSeq<'_> {
        self.as_slice().slice(range).to_owned()
    }

    pub fn push_ascii(&mut self, seq: &[u8]) -> Range<usize> {
        let r1 = self.seq.push_ascii(seq);
        let r2 = self.ambiguous.push_ascii(seq);
        assert_eq!(r1, r2);
        r1
    }

    pub fn from_ascii(seq: &[u8]) -> Self {
        Self {
            seq: PackedSeqVec::from_ascii(seq),
            ambiguous: BitSeqVec::from_ascii(seq),
        }
    }

    /// Create a mask that is 1 for all non-ACGT bases and for all low-quality bases with quality `<threshold`.
    pub fn from_ascii_and_quality(seq: &[u8], quality: &[u8], threshold: usize) -> Self {
        assert_eq!(seq.len(), quality.len());

        let mut ambiguous = BitSeqVec::from_ascii(seq);

        // Low-quality bases are also ambiguous.
        {
            let t = (b'!' + threshold as u8) as i8;
            let t_simd = i8x32::splat(t);
            let ambiguous = ambiguous.seq.as_chunks_mut::<4>().0;
            for i in (0..quality.len()).step_by(32) {
                let chunk = i8x32::from(unsafe {
                    std::mem::transmute::<_, i8x32>(read_slice_32(quality, i))
                });

                let mask = t_simd.cmp_lt(chunk).move_mask() as u32;
                let ambi = &mut ambiguous[i / 32];
                *ambi = (u32::from_ne_bytes(*ambi) | mask).to_ne_bytes();
            }
        }

        Self {
            seq: PackedSeqVec::from_ascii(seq),
            ambiguous,
        }
    }

    /// Create a mask that is 1 for all non-ACGT bases and for all low-quality bases with quality `<threshold`.
    pub fn push_from_ascii_and_quality(&mut self, seq: &[u8], quality: &[u8], min_qual: u8) {
        let r = self.seq.push_ascii(seq);
        let r2 = self.ambiguous.push_ascii(seq);
        assert_eq!(r, r2);

        assert_eq!(seq.len(), quality.len());

        // Low-quality bases are also ambiguous.
        let t = b'!' + min_qual;
        let t_simd = i8x32::splat(t as i8);

        let mut idx = r2.start;
        let mut i = 0;
        while idx % 8 != 0 {
            self.ambiguous.seq[idx / 8] |= ((quality[i] < t) as u8) << (idx % 8);
            idx += 1;
            i += 1;
        }
        let quality = &quality[i..];

        let ambiguous = self.ambiguous.seq[idx / 8..].as_chunks_mut::<4>().0;
        for i in (0..quality.len()).step_by(32) {
            let chunk =
                i8x32::from(unsafe { std::mem::transmute::<_, i8x32>(read_slice_32(quality, i)) });

            let mask = t_simd.cmp_lt(chunk).move_mask() as u32;
            let ambi = &mut ambiguous[i / 32];
            *ambi = (u32::from_ne_bytes(*ambi) | mask).to_ne_bytes();
        }
    }

    /// Read a fasta/fastq file at the given path into a `PackedNSeq`, separating records with `N`.
    #[cfg(feature = "file_io")]
    pub fn from_fastx(path: &std::path::Path) -> Self {
        let mut ascii = Vec::with_capacity(16000);

        let mut seq = Self::default();

        let mut reader = needletail::parse_fastx_file(&path).unwrap();
        while let Some(record) = reader.next() {
            let seqrec = record.expect("Invalid FASTA/Q record");
            ascii.extend_from_slice(&seqrec.seq());
            ascii.push(b'N');

            // Convert from ascii dna to packed dna+mask in batches of 16kbp.
            if ascii.len() > 16000 {
                seq.push_ascii(&ascii);
                ascii.clear();
            }
        }

        if ascii.len() > 0 {
            seq.push_ascii(&ascii);
        }

        seq
    }

    /// Read a fasta/fastq file at the given path into a `PackedNSeq`, separating records with `N`.
    /// Low-quality bases are masked out.
    #[cfg(feature = "file_io")]
    pub fn from_fastq_with_quality(path: &std::path::Path, min_qual: u8) -> Self {
        let mut ascii = vec![];
        let mut qual = vec![];

        let mut seq = Self::default();

        let mut reader = needletail::parse_fastx_file(&path).unwrap();
        while let Some(record) = reader.next() {
            let seqrec = record.expect("Invalid FASTA/Q record");
            ascii.extend_from_slice(&seqrec.seq());
            qual.extend_from_slice(&seqrec.qual().unwrap());
            ascii.push(b'N');
            qual.push(0);

            // Convert from ascii dna+qual to packed dna+mask in batches of 16kbp.
            if ascii.len() > 16000 {
                seq.push_from_ascii_and_quality(&ascii, &qual, min_qual);
                ascii.clear();
                qual.clear();
            }
        }

        if ascii.len() > 0 {
            seq.push_from_ascii_and_quality(&ascii, &qual, min_qual);
        }

        seq
    }
}
