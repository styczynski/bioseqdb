#![feature(portable_simd)]

use itertools::Itertools;

pub mod counting;
pub mod fxhash;
pub mod hash;
pub mod jumping;
pub mod minimizer;
pub mod naive;
#[cfg(target_feature = "avx")]
pub mod nthash;
pub mod queue;
pub mod queue_igor;
pub mod rescan;
pub mod rescan_daniel;
pub mod ringbuf;
pub mod sliding_min;
pub mod split;

pub use fxhash::*;
pub use hash::*;
pub use jumping::*;
pub use minimizer::*;
pub use naive::*;
#[cfg(target_feature = "avx")]
pub use nthash::*;
pub use queue::*;
pub use queue_igor::*;
pub use rescan::*;
pub use rescan_daniel::*;
pub use ringbuf::*;
pub use sliding_min::*;
pub use split::*;

use packed_seq::{PackedSeqVec, SeqVec};

pub(crate) const MAXIMUM_K_SIZE: usize = u32::max_value() as usize;

pub trait Max {
    const MAX: Self;
}

impl Max for usize {
    const MAX: usize = usize::MAX;
}
impl Max for u64 {
    const MAX: u64 = u64::MAX;
}

pub fn read_human_genome(chromosomes: usize) -> Vec<PackedSeqVec> {
    eprintln!("Reading..");
    let start = std::time::Instant::now();
    let mut out = vec![];
    let Ok(mut reader) = needletail::parse_fastx_file("human-genome.fa") else {
        eprintln!(
            "Did not find human-genome.fa. Add/symlink it to test runtime on it.
Download and then unzip it from the first link here: https://github.com/marbl/CHM13?tab=readme-ov-file#t2t-chm13v20-t2t-chm13y
"
        );
        return out;
    };
    let mut i = 0;
    while let Some(r) = reader.next() {
        let r = r.unwrap();
        // eprintln!(
        //     "Read {:?} of len {:?}",
        //     std::str::from_utf8(r.id()),
        //     r.seq().len()
        // );
        out.push(PackedSeqVec::from_ascii(&r.seq()));
        i += 1;
        if i == chromosomes {
            break;
        }
    }
    eprintln!(
        "Reading & packing human-genome.fa took {:?}",
        start.elapsed()
    );
    out
}
