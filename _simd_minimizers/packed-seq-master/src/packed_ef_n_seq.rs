use mem_dbg::{MemDbg, MemSize};
use sux::dict::{EliasFano, EliasFanoBuilder};

use crate::{BitSeqVec, PackedNSeqVec, PackedSeqVec, SeqVec};

/// Like the 2+1-bit encoded [`PackedNSeqVec`], but using Elias-Fano to encode the positions of the N characters.
/// Note that the fixed-cost overhead of [`sux::dict::EliasFano`] is quite large at ~5 `usize` values, or 40 bytes (=160bp).
#[derive(Clone, Debug, MemSize, MemDbg)]
#[cfg_attr(feature = "pyo3", pyo3::pyclass)]
#[cfg_attr(feature = "epserde", derive(epserde::Epserde))]
pub struct PackedEfNSeqVec {
    seq: PackedSeqVec,
    ef_ambiguous: Option<EliasFano>,
}

impl PackedEfNSeqVec {
    pub fn from_packed_n_seq_vec(n_seq: PackedNSeqVec) -> Self {
        let mut cnt = 0;
        let (chunks, tail) = n_seq.ambiguous.seq.as_chunks();
        for chunk in chunks {
            cnt += u64::from_ne_bytes(*chunk).count_ones();
        }
        for byte in tail {
            cnt += byte.count_ones();
        }

        if cnt == 0 {
            return Self {
                seq: n_seq.seq,
                ef_ambiguous: None,
            };
        }

        let mut ef_builder = EliasFanoBuilder::new(cnt as usize, n_seq.seq.seq.len() as usize);
        let mut pos = 0;
        for mut byte in n_seq.ambiguous.seq {
            while byte > 0 {
                let i = byte.count_zeros();
                ef_builder.push(pos + i as usize);
                byte ^= 1 << i;
            }
            pos += 8;
        }
        Self {
            seq: n_seq.seq,
            ef_ambiguous: Some(ef_builder.build()),
        }
    }
    pub fn into_packed_n_seq_vec(self) -> PackedNSeqVec {
        let mut ambiguous = BitSeqVec::with_len(self.seq.len());
        if let Some(ef_ambiguous) = self.ef_ambiguous {
            for pos in ef_ambiguous.iter() {
                ambiguous.seq[pos / 8] |= 1 << (pos % 8);
            }
        }
        PackedNSeqVec {
            seq: self.seq,
            ambiguous,
        }
    }
}

#[test]
fn ef_duplicate_values() {
    let vals = vec![1, 2, 3, 4, 8, 10, 10, 123];
    let mut builder = EliasFanoBuilder::new(vals.len(), *vals.last().unwrap());
    for x in vals {
        builder.push(x);
    }
    builder.build();
}
