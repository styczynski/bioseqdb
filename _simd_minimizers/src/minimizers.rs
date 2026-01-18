//! Find the (canonical) minimizers of a sequence.
use std::iter::zip;

use crate::{
    canonical,
    sliding_min::{Cache, sliding_lr_min_mapper_scalar, sliding_min_mapper_scalar},
};

use super::{
    canonical::canonical_mapper_simd,
    sliding_min::{sliding_lr_min_mapper_simd, sliding_min_mapper_simd},
};
use itertools::{Itertools, izip};
use packed_seq::{Advance, ChunkIt, Delay, PaddedIt, Seq};
use seq_hash::KmerHasher;
use wide::u32x8;

pub const SKIPPED: u32 = u32::MAX - 1;
pub(crate) const SIMD_SKIPPED: u32x8 = u32x8::new([SKIPPED; 8]);

/// Minimizer position of a single window.
pub fn one_minimizer<'s>(seq: impl Seq<'s>, hasher: &impl KmerHasher) -> usize {
    hasher
        .hash_kmers_scalar(seq)
        .map(|x| x & 0xffff_0000)
        .position_min()
        .unwrap()
}

// FIMXE: Add one_canonical_minimizer

/// Returns an iterator over the absolute positions of the minimizers of a sequence.
/// Returns one value for each window of size `w+k-1` in the input. Use
/// `Itertools::dedup()` to obtain the distinct positions of the minimizers.
///
/// Prefer `minimizer_simd_it` that internally used SIMD, or `minimizer_par_it` if it works for you.
#[inline(always)]
pub fn minimizers_seq_scalar<'s>(
    seq: impl Seq<'s>,
    hasher: &impl KmerHasher,
    w: usize,
    cache: &mut Cache,
) -> impl ExactSizeIterator<Item = u32> {
    let kmer_hashes = hasher.hash_kmers_scalar(seq);
    let len = kmer_hashes.len();
    kmer_hashes
        .map(sliding_min_mapper_scalar::<true>(w, len, cache))
        .advance(w - 1)
}

/// Split the windows of the sequence into 8 chunks of equal length ~len/8.
/// Then return the positions of the minimizers of each of them in parallel using SIMD,
/// and return the remaining few using the second iterator.
#[inline(always)]
pub fn minimizers_seq_simd<'s>(
    seq: impl Seq<'s>,
    hasher: &impl KmerHasher,
    w: usize,
    cache: &mut Cache,
) -> PaddedIt<impl ChunkIt<u32x8>> {
    let kmer_hashes = hasher.hash_kmers_simd(seq, w);
    let len = kmer_hashes.it.len();
    kmer_hashes
        .map(sliding_min_mapper_simd::<true>(w, len, cache))
        .advance(w - 1)
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// TRULY CANONICAL MINIMIZERS BELOW HERE
// The minimizers above can take a canonical hash, but do not correctly break ties.
// Below we fix that.

#[inline(always)]
pub fn canonical_minimizers_seq_scalar<'s>(
    seq: impl Seq<'s>,
    hasher: &impl KmerHasher,
    w: usize,
    cache: &mut Cache,
) -> impl ExactSizeIterator<Item = u32> {
    // TODO: Change to compile-time check on `impl KmerHasher<RC=true>` once supported.
    assert!(hasher.is_canonical());

    let k = hasher.k();
    let delay1 = hasher.delay().0;
    let mut hash_mapper = hasher.in_out_mapper_scalar(seq);
    let mut sliding_min_mapper = sliding_lr_min_mapper_scalar(w, seq.len(), cache);
    let (Delay(delay2), mut canonical_mapper) = canonical::canonical_mapper_scalar(k + w - 1);

    assert!(delay1 < k);
    assert!(k - 1 <= delay2);
    assert!(delay2 == k + w - 2);

    let mut a = seq.iter_bp();
    let mut rh = seq.iter_bp();
    let rc = seq.iter_bp();

    a.by_ref().take(delay1).for_each(|a| {
        hash_mapper((a, 0));
        canonical_mapper((a, 0));
    });

    zip(a.by_ref(), rh.by_ref())
        .take((k - 1) - delay1)
        .for_each(|(a, rh)| {
            hash_mapper((a, rh));
            canonical_mapper((a, 0));
        });

    zip(a.by_ref(), rh.by_ref())
        .take(delay2 - (k - 1))
        .for_each(|(a, rh)| {
            let hash = hash_mapper((a, rh));
            canonical_mapper((a, 0));
            sliding_min_mapper(hash);
        });

    izip!(a, rh, rc).map(
        #[inline(always)]
        #[allow(clippy::let_and_return)]
        move |(a, rh, rc)| {
            let hash = hash_mapper((a, rh));
            let canonical = canonical_mapper((a, rc));
            let (left, right) = sliding_min_mapper(hash);
            // Assigning to x ensures we get a cmov here.
            let x = if canonical { left } else { right };
            x
        },
    )
}

/// Use canonical NtHash, and keep both leftmost and rightmost minima.
#[inline(always)]
pub fn canonical_minimizers_seq_simd<'s>(
    seq: impl Seq<'s>,
    hasher: &impl KmerHasher,
    w: usize,
    cache: &mut Cache,
) -> PaddedIt<impl ChunkIt<u32x8>> {
    assert!(hasher.is_canonical());

    let k = hasher.k();
    let l = k + w - 1;
    let mut hash_mapper = hasher.in_out_mapper_simd(seq);
    let (c_delay, mut canonical_mapper) = canonical_mapper_simd(l);

    let mut padded_it = seq.par_iter_bp_delayed_2(l, hasher.delay(), c_delay);

    // Process first k-1 characters separately, to initialize hash values.
    padded_it.advance_with(k - 1, |(a, rh, rc)| {
        hash_mapper((a, rh));
        canonical_mapper((a, rc));
    });
    let mut sliding_min_mapper = sliding_lr_min_mapper_simd(w, padded_it.it.len(), cache);
    padded_it.advance_with(w - 1, |(a, rh, rc)| {
        let hash = hash_mapper((a, rh));
        canonical_mapper((a, rc));
        sliding_min_mapper(hash);
    });

    padded_it.map(move |(a, rh, rc)| {
        let hash = hash_mapper((a, rh));
        let canonical = canonical_mapper((a, rc));
        let (lmin, rmin) = sliding_min_mapper(hash);
        canonical.blend(lmin, rmin)
    })
}

#[inline(always)]
pub fn canonical_minimizers_skip_ambiguous_windows<'s>(
    nseq: packed_seq::PackedNSeq<'s>,
    hasher: &impl KmerHasher,
    w: usize,
    cache: &'s mut (Cache, Vec<u32x8>, Vec<u32x8>),
) -> PaddedIt<impl ChunkIt<u32x8>> {
    assert!(hasher.is_canonical());

    let k = hasher.k();
    let l = k + w - 1;
    let mut hash_mapper = hasher.in_out_mapper_simd(nseq.seq);
    let (c_delay, mut canonical_mapper) = canonical_mapper_simd(l);

    let mut padded_it = nseq.seq.par_iter_bp_delayed_2_with_factor_and_buf(
        l,
        hasher.delay(),
        c_delay,
        2,
        &mut cache.1,
    );

    // Process first k-1 characters separately, to initialize hash values.
    padded_it.advance_with(k - 1, |(a, rh, rc)| {
        hash_mapper((a, rh));
        canonical_mapper((a, rc));
    });
    let mut sliding_min_mapper = sliding_lr_min_mapper_simd(w, padded_it.it.len(), &mut cache.0);
    padded_it.advance_with(w - 1, |(a, rh, rc)| {
        let hash = hash_mapper((a, rh));
        canonical_mapper((a, rc));
        sliding_min_mapper(hash);
    });

    // jump over the l-1 first ambiguity results
    padded_it
        .zip(
            nseq.ambiguous
                .par_iter_kmer_ambiguity_with_buf(l, l, l - 1, &mut cache.2),
        )
        .map(move |((a, rh, rc), ambi)| {
            let hash = hash_mapper((a, rh));
            let canonical = canonical_mapper((a, rc));
            let (lmin, rmin) = sliding_min_mapper(hash);
            ambi.blend(SIMD_SKIPPED, canonical.blend(lmin, rmin))
        })
}
